/*-------------------------------------------------------------------------
 *
 * shm_mq_pool.c
 * 		pooling for shm_mq
 *
 * Copyright (c) 2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  shm_mq_pool.c
 *
 *-------------------------------------------------------------------------
 *
 * This module provides pools shm_mq pairs that can be used by processes
 * to communicate between each other.
 *
 * The processes can attach to the pool as needed which activates the queue
 * pair and the remote side will start processing it.
 */
#include "postgres.h"

#include "miscadmin.h"

#include "access/twophase.h"

#include "lib/stringinfo.h"

#include "nodes/pg_list.h"

#include "pgstat.h"

#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/shm_mq.h"
#include "storage/spin.h"

#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

#include "shm_mq_pool.h"

struct MQPoolConn
{
	/* Pointer back to owning pool. */
	dsa_pointer		mqpool;

	/* Connected client backend. */
	PGPROC		   *client;

	/* DSA pointers to queues. */
	dsa_pointer		clientq;
	dsa_pointer		serverq;

	/*
	 * Process local queue attached handles.
	 * TODO: move to process local memory.
	 */
	shm_mq_handle  *client_recvqh;
	shm_mq_handle  *client_sendqh;
	shm_mq_handle  *server_recvqh;
	shm_mq_handle  *server_sendqh;
};

struct MQPool
{
	/* Write lock. */
	slock_t			mutex;

	/* Name of the pool. */
	NameData		name;

	/* This is pointer to shmem. */
	PGPROC		   *owner;

	/* Size of the queue buffer for each connection. */
	Size			recv_queue_size;

	/* Pooler local callbacks (only valid in the pooler proccess0. */
	shm_mq_pool_connect_cb connect_cb;
	shm_mq_pool_disconnect_cb disconnect_cb;
	shm_mq_pool_message_cb message_cb;

	slist_head      waiters;

	/* Number of connections in the pool. */
	uint32			max_connections;

	/* Allocated connections. */
	uint32			alloc_connections;

	/* Connections in the pool. */
	dsa_pointer		connections[FLEXIBLE_ARRAY_MEMBER];
};

typedef struct MQPooler
{
	int				npools;
	dsa_pointer		pools[FLEXIBLE_ARRAY_MEMBER];
} MQPooler;

typedef struct MQPoolWaiter {
	PGPROC		   *proc;
	slist_node		node;
} MQPoolWaiter;

struct MQPoolerContext
{
	/* Write lock. */
	slock_t			mutex;
	dsa_handle		dsa;
	dsa_pointer		pooler;

	MQPoolWaiter   *waiters;
};

MQPoolerContext			   *MQPoolerCtx = NULL;

static dsa_area			   *MQPoolerDsaArea = NULL;

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static dsa_pointer dsa_reallocate(dsa_area *dsa, dsa_pointer olddp, Size oldsz, Size newsz);

/*
 * Init shmem needed our context.
 */
static void
shm_mq_pooler_shm_startup(void)
{
	bool        found;

	/* See InitProcGlobal() */
	uint32		TotalProcs = MaxBackends + NUM_AUXILIARY_PROCS + max_prepared_xacts;

	if (prev_shmem_startup_hook != NULL)
		prev_shmem_startup_hook();

	/* Init signaling context for supervisor proccess. */
	MQPoolerCtx = ShmemInitStruct("shm_mq_pooler", sizeof(MQPoolerContext) +
								  sizeof(MQPoolWaiter) * TotalProcs,
								  &found);

	if (!found)
	{
		memset(MQPoolerCtx, 0,
			   sizeof(MQPoolerContext) + sizeof(MQPoolWaiter) * TotalProcs);
		SpinLockInit(&MQPoolerCtx->mutex);
		MQPoolerCtx->waiters = (MQPoolWaiter *) (MQPoolerCtx + sizeof(MQPoolerContext));
	}
}

/*
 * Request shared memory and locks
 *
 * Called by postmaster
 */
void
shm_mq_pooler_shmem_init(void)
{
	Assert(process_shared_preload_libraries_in_progress);

	RequestNamedLWLockTranche("shm_mq_pooler", 1);

	RequestAddinShmemSpace(sizeof(MQPoolerContext));

	MQPoolerCtx = NULL;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = shm_mq_pooler_shm_startup;
}

/*
 * Initialize the pooler, called first time pool creation is requested.
 */
static void
shm_mq_pooler_init(void)
{
	LWLockPadded	   *lock;
	dsa_pointer			dp;
	MQPooler		   *pooler;

	elog(DEBUG1, "initializing shm_mq_pooler");

	lock = GetNamedLWLockTranche("shm_mq_pooler");

	MQPoolerDsaArea = dsa_create(lock->lock.tranche);

	MQPoolerCtx->dsa = dsa_get_handle(MQPoolerDsaArea);

	dp = dsa_allocate0(MQPoolerDsaArea, sizeof(MQPooler));
	pooler = dsa_get_address(MQPoolerDsaArea, dp);
	pooler->npools = 0;

	MQPoolerCtx->pooler = dp;
}

static bool
shm_mq_pool_conn_used(MQPoolConn *mqconn)
{
	shm_mq	   *mq;

	if (mqconn->clientq)
	{
		mq = dsa_get_address(MQPoolerDsaArea, mqconn->clientq);
		if (shm_mq_get_receiver(mq) || shm_mq_get_sender(mq))
			return true;
	}

	if (mqconn->serverq)
	{
		mq = dsa_get_address(MQPoolerDsaArea, mqconn->serverq);
		if (shm_mq_get_receiver(mq) || shm_mq_get_sender(mq))
			return true;
	}

	return false;
}

static void
shm_mq_pool_on_server_detach(dsm_segment *seg, Datum arg)
{
	MQPool	   *mqpool = (MQPool *) DatumGetPointer(arg);

	SpinLockAcquire(&mqpool->mutex);
	mqpool->owner = NULL;
	SpinLockRelease(&mqpool->mutex);
}

/*
 * Register new pool
 *
 * TODO: improve locking
 */
MQPool *
shm_mq_pooler_new_pool(const char *name, int max_connections, Size recv_queue_size,
					   shm_mq_pool_connect_cb connect_cb,
					   shm_mq_pool_disconnect_cb disconnect_cb,
					   shm_mq_pool_message_cb message_cb)
{
	dsa_pointer			dp;
	dsa_pointer			mqpoolp;
	MQPool			   *mqpool = NULL;
	int					i;
	MQPooler		   *pooler;
	ResourceOwner		oldresowner = CurrentResourceOwner;
	MemoryContext		oldctx = MemoryContextSwitchTo(TopMemoryContext);

	CurrentResourceOwner = NULL;

	elog(DEBUG1, "creating new shm_mq_pool \"%s\"", name);
	/* Is the module initialized? */
	if (MQPoolerCtx == NULL)
		elog(ERROR, "shm_mq_pooler not initialized");

	SpinLockAcquire(&MQPoolerCtx->mutex);

	/* Init the pooler if needed. */
	if (!DsaPointerIsValid(MQPoolerCtx->pooler))
		shm_mq_pooler_init();

	/* Build local caches if needed. */
	if (MQPoolerDsaArea == NULL)
		MQPoolerDsaArea = dsa_attach(MQPoolerCtx->dsa);

	/* Check for name conflict. */
	pooler = dsa_get_address(MQPoolerDsaArea, MQPoolerCtx->pooler);

	for (i = 0; i < pooler->npools; i++)
	{
		mqpool = dsa_get_address(MQPoolerDsaArea, pooler->pools[i]);

		/* Potentially unused pool, check if we can reuse it already. */
		SpinLockAcquire(&mqpool->mutex);
		if (!mqpool->owner && mqpool->alloc_connections >= max_connections)
		{
			int		ci;

			/*
			 * We only care about connections up to max_connections even
			 * if the pool might have more allocated.
			 */
			for (ci = 0; ci < max_connections; ci++)
			{
				MQPoolConn   *mqconn = dsa_get_address(MQPoolerDsaArea,
													   mqpool->connections[ci]);
				if (shm_mq_pool_conn_used(mqconn))
					break;
			}

			/*
			 * Found unused pool, re-use it.
			 * NB: we don't release the mutex lock here on purpose.
			 */
			if (ci == max_connections)
			{
				mqpoolp = pooler->pools[i];
				break;
			}
		}
		/* Report used pool with same name. */
		else if (namestrcmp(&mqpool->name, name) == 0)
		{
			SpinLockRelease(&mqpool->mutex);
			elog(ERROR, "pool \"%s\" already exists", name);
		}

		SpinLockRelease(&mqpool->mutex);
		mqpool = NULL;
	}

	if (!mqpool)
	{
		mqpoolp = dsa_allocate0(MQPoolerDsaArea,
								sizeof(MQPool) + sizeof(dsa_pointer) * max_connections);
		mqpool = dsa_get_address(MQPoolerDsaArea, mqpoolp);
		SpinLockInit(&mqpool->mutex);

		mqpool->alloc_connections = max_connections;

		for (i = 0; i < mqpool->alloc_connections; i++)
		{
			MQPoolConn	   *mqconn;

			dp = dsa_allocate0(MQPoolerDsaArea, sizeof(MQPoolConn));
			mqconn = dsa_get_address(MQPoolerDsaArea, dp);
			mqconn->mqpool = mqpoolp;

			mqpool->connections[i] = dp;
		}

		MQPoolerCtx->pooler =
			dsa_reallocate(MQPoolerDsaArea, MQPoolerCtx->pooler,
						   sizeof(MQPooler) + pooler->npools * sizeof(dsa_pointer),
						   sizeof(MQPooler) + (pooler->npools + 1) * sizeof(dsa_pointer));
		pooler = dsa_get_address(MQPoolerDsaArea, MQPoolerCtx->pooler);

		SpinLockAcquire(&mqpool->mutex);

		pooler->pools[pooler->npools] = mqpoolp;
		pooler->npools++;

	}

	on_dsm_detach(dsm_find_mapping(dsa_get_handle(MQPoolerDsaArea)),
				  shm_mq_pool_on_server_detach, PointerGetDatum(mqpool));

	Assert(strlen(name) <= NAMEDATALEN);
	memcpy(NameStr(mqpool->name), name, NAMEDATALEN);
	mqpool->owner = MyProc;
	mqpool->recv_queue_size = recv_queue_size;
	mqpool->connect_cb = connect_cb;
	mqpool->disconnect_cb = disconnect_cb;
	mqpool->message_cb = message_cb;
	mqpool->max_connections = max_connections;

	SpinLockRelease(&mqpool->mutex);

	SpinLockRelease(&MQPoolerCtx->mutex);

	MemoryContextSwitchTo(oldctx);
	CurrentResourceOwner = oldresowner;

	return mqpool;
}

static void
shm_mq_pool_on_client_detach(dsm_segment *seg, Datum arg)
{
	MQPoolConn	   *mqconn = (MQPoolConn *) DatumGetPointer(arg);

	shm_mq_pool_disconnect(mqconn);
}

/*
 * Attach to queues and register sender/receiver.
 */
static void
shm_mq_pool_attach_connection(MQPool *mqpool, MQPoolConn *mqconn)
{
	shm_mq		   *clientq;
	shm_mq		   *serverq;

	clientq = dsa_get_address(MQPoolerDsaArea, mqconn->clientq);
	serverq = dsa_get_address(MQPoolerDsaArea, mqconn->serverq);

	if (mqpool->owner == MyProc)
	{
		shm_mq_set_receiver(serverq, MyProc);
		mqconn->server_recvqh = shm_mq_attach(serverq, NULL, NULL);
		shm_mq_set_sender(clientq, MyProc);
		mqconn->server_sendqh = shm_mq_attach(clientq, NULL, NULL);
	}
	else
	{
		dsm_segment	   *seg;

		/* DSA handle is handle of its control DSM segment. */
		seg = dsm_find_mapping(dsa_get_handle(MQPoolerDsaArea));
		if (!seg)
			seg = dsm_attach(dsa_get_handle(MQPoolerDsaArea));

		/* Make sure we run cleanup on detach. */
		on_dsm_detach(seg, shm_mq_pool_on_client_detach, PointerGetDatum(mqconn));

		shm_mq_set_receiver(clientq, MyProc);
		mqconn->client_recvqh = shm_mq_attach(clientq, NULL, NULL);
		shm_mq_set_sender(serverq, MyProc);
		mqconn->client_sendqh = shm_mq_attach(serverq, NULL, NULL);

		/* Signal the pool owner that there is new connection. */
		SetLatch(&mqpool->owner->procLatch);
	}
}

/*
 * Event loop callback for the pooler (like select())
 *
 * Called should be called by proccess which registered any pool.
 * TODO: locking
 */
void
shm_mq_pooler_work(void)
{
	int			pi;
	MQPooler   *pooler;

	/*
	 * If MQPoolerDsaArea is empty, nobody registered any pool in this
	 * proccess.
	 */
	if (MQPoolerDsaArea == NULL)
		return;

	pooler = dsa_get_address(MQPoolerDsaArea, MQPoolerCtx->pooler);

	for (pi = 0; pi < pooler->npools; pi++)
	{
		MQPool	   *mqpool = dsa_get_address(MQPoolerDsaArea,
											 pooler->pools[pi]);
		Size		nbytes;
		void	   *data;
		int			ci;
		shm_mq_result	result;

		if (mqpool->owner != MyProc)
			continue;

		for (ci = 0; ci < mqpool->max_connections; ci++)
		{
			MQPoolConn	   *mqconn = dsa_get_address(MQPoolerDsaArea,
													 mqpool->connections[ci]);
			shm_mq		   *mq;

			CHECK_FOR_INTERRUPTS();

			if (!shm_mq_pool_conn_used(mqconn))
				continue;

			/*
			 * Attach to connection if not attached already (this is a new
			 * connection).
			 */
			mq = dsa_get_address(MQPoolerDsaArea, mqconn->serverq);
			if (!shm_mq_get_receiver(mq))
			{
				shm_mq_pool_attach_connection(mqpool, mqconn);
				if (mqpool->connect_cb)
					mqpool->connect_cb(mqconn);
			}

			/* Attempt to read a message. */
			result = shm_mq_receive(mqconn->server_recvqh, &nbytes, &data, true);

			/* Client has disconnected. */
			if (result == SHM_MQ_DETACHED)
			{
				if (mqpool->disconnect_cb)
					mqpool->disconnect_cb(mqconn);

				/* Cleanup the connection info. */
				SpinLockAcquire(&mqpool->mutex);
				shm_mq_detach(mqconn->server_recvqh);
				mqconn->server_recvqh = NULL;
				shm_mq_detach(mqconn->server_sendqh);
				mqconn->server_sendqh = NULL;
				dsa_free(MQPoolerDsaArea, mqconn->clientq);
				dsa_free(MQPoolerDsaArea, mqconn->serverq);
				mqconn->client_recvqh = NULL;
				mqconn->client_sendqh = NULL;
				mqconn->clientq = 0;
				mqconn->serverq = 0;
				SpinLockRelease(&mqpool->mutex);

				/* Signal all waiters for connection slot. */
				while (!slist_is_empty(&mqpool->waiters))
				{
					slist_node	   *node;
					MQPoolWaiter   *waiter;
					PGPROC		   *proc;

					SpinLockAcquire(&mqpool->mutex);
					node = slist_pop_head_node(&mqpool->waiters);
					SpinLockRelease(&mqpool->mutex);
					waiter = slist_container(MQPoolWaiter, node, node);
					proc = waiter->proc;

					SetLatch(&proc->procLatch);
				}
			}

			if (result != SHM_MQ_SUCCESS)
				continue;

			mqpool->message_cb(mqconn, data, nbytes);
		}
	}
}

/*
 * Get connection from pool.
 *
 * Called by backed (normal or bgworker) which wants to communicate with the
 * proccess serving the given pool.
 */
MQPoolConn *
shm_mq_pool_get_connection(MQPool *mqpool, bool nowait)
{
	int				i;
    MQPoolWaiter   *waiter = NULL;

retry:
	for (i = 0; i < mqpool->max_connections; i++)
	{
		dsa_pointer		dp = mqpool->connections[i];
		MQPoolConn	   *mqconn = dsa_get_address(MQPoolerDsaArea, dp);
		shm_mq		   *mq;
		void		   *queueaddr;
		MemoryContext oldctx;

		SpinLockAcquire(&mqpool->mutex);

		/* Check if the connection is free. */
		if (shm_mq_pool_conn_used(mqconn))
		{
			SpinLockRelease(&mqpool->mutex);
			continue;
		}

		mqconn->client = MyProc;

		oldctx = MemoryContextSwitchTo(TopMemoryContext);

		/* Create queues. */
		mqconn->clientq = dsa_allocate(MQPoolerDsaArea, mqpool->recv_queue_size);
		queueaddr = dsa_get_address(MQPoolerDsaArea, mqconn->clientq);
		mq = shm_mq_create(queueaddr, mqpool->recv_queue_size);
		Assert(queueaddr == mq);

		mqconn->serverq = dsa_allocate(MQPoolerDsaArea, mqpool->recv_queue_size);
		queueaddr = dsa_get_address(MQPoolerDsaArea, mqconn->serverq);
		mq = shm_mq_create(queueaddr, mqpool->recv_queue_size);
		Assert(queueaddr == mq);

		/* Attach proccess to the connection. */
		shm_mq_pool_attach_connection(mqpool, mqconn);

		SpinLockRelease(&mqpool->mutex);

		MemoryContextSwitchTo(oldctx);

		return mqconn;
	}

	/* Wait if requested. */
	if (!nowait)
	{
		int rc;

		CHECK_FOR_INTERRUPTS();

		/* If this is the first time we tried, add us to the waiter list. */
		if (waiter == NULL)
		{
			waiter = &MQPoolerCtx->waiters[MyProc->pgprocno];
			waiter->proc = MyProc;
			SpinLockAcquire(&mqpool->mutex);
			slist_push_head(&mqpool->waiters, &waiter->node);
			SpinLockRelease(&mqpool->mutex);
		}

		/* Retry after maximum of 1s. */
        rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 1000L,
					   PG_WAIT_EXTENSION);
		ResetLatch(&MyProc->procLatch);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		goto retry;
	}

	return NULL;
}

/*
 * Get pool based on pool id.
 *
 * Called by backed (normal or bgworker) which wants to communicate with the
 * proccess serving the given pool.
 */
MQPool *
shm_mq_pool_get_pool(const char *name)
{
	MQPool		   *mqpool = NULL;
	MQPooler	   *pooler;
	int				i;

	/* CHeck if module has been initialized. */
	if (MQPoolerCtx == NULL)
		elog(ERROR, "shm_mq_pooler not initialized");

	/* If there are no pools we can't find any. */
	if (!DsaPointerIsValid(MQPoolerCtx->pooler))
		return NULL;

	/* Build local cache if needed. */
	if (MQPoolerDsaArea == NULL)
	{
		ResourceOwner	oldresowner;
		MemoryContext	oldctx;

		oldresowner = CurrentResourceOwner;
		CurrentResourceOwner = NULL;
		oldctx = MemoryContextSwitchTo(TopMemoryContext);

		MQPoolerDsaArea = dsa_attach(MQPoolerCtx->dsa);

		MemoryContextSwitchTo(oldctx);
		CurrentResourceOwner = oldresowner;
	}

	pooler = dsa_get_address(MQPoolerDsaArea, MQPoolerCtx->pooler);
	for (i = 0; i < pooler->npools; i++)
	{
		mqpool = dsa_get_address(MQPoolerDsaArea, pooler->pools[i]);
		if (mqpool->owner && namestrcmp(&mqpool->name, name) == 0)
			break;
		else
			mqpool = NULL;
	}

	return mqpool;
}

void
shm_mq_pool_disconnect(MQPoolConn *mqconn)
{
	dsm_segment	   *seg;
	MQPool		   *mqpool;

	seg = dsm_find_mapping(dsa_get_handle(MQPoolerDsaArea));
	cancel_on_dsm_detach(seg, shm_mq_pool_on_client_detach, PointerGetDatum(mqconn));

	mqpool = dsa_get_address(MQPoolerDsaArea, mqconn->mqpool);

	SpinLockAcquire(&mqpool->mutex);
	shm_mq_detach(mqconn->client_recvqh);
	mqconn->client_recvqh = NULL;
	shm_mq_detach(mqconn->client_sendqh);
	mqconn->client_sendqh = NULL;
	mqconn->client = NULL;
	SpinLockRelease(&mqpool->mutex);
}

/*
 * Write to connection.
 */
bool
shm_mq_pool_write(MQPoolConn *mqconn, StringInfo msg)
{
	shm_mq_result	result;
	shm_mq_handle  *sendqh = mqconn->client == MyProc ? mqconn->client_sendqh :
		mqconn->server_sendqh;

	result = shm_mq_send(sendqh, msg->len, msg->data, false);

	return (result == SHM_MQ_SUCCESS);
}

/*
 * (Try to) read from connection.
 */
bool
shm_mq_pool_receive(MQPoolConn *mqconn, StringInfo output, bool nowait)
{
	shm_mq_result	result;
	Size			nbytes;
	void		   *data;
	shm_mq_handle  *recvqh = mqconn->client == MyProc ? mqconn->client_recvqh :
		mqconn->server_recvqh;

	for (;;)
	{
		/* Attempt to read a message. */
		result = shm_mq_receive(recvqh, &nbytes, &data, nowait);

		if (result == SHM_MQ_DETACHED)
			return false;

		/* This is only returned when nowait is fase. */
		if (result == SHM_MQ_WOULD_BLOCK)
		{
			output->len = 0;
			return true;
		}

		Assert(result == SHM_MQ_SUCCESS);

		output->data = data;
		output->len	= nbytes;

		return true;
	}

	return false; /* unreachable */
}

/*
 * The DSA in PostgreSQL does not support realloc so we implement our own.
 */
static dsa_pointer
dsa_reallocate(dsa_area *dsa, dsa_pointer olddp, Size oldsz, Size newsz)
{
	dsa_pointer		dp;
	char		   *oldptr;
	char		   *newptr;

	oldptr = dsa_get_address(MQPoolerDsaArea, olddp);

	dp = dsa_allocate(dsa, newsz);
	newptr = dsa_get_address(MQPoolerDsaArea, dp);

	memcpy(newptr, oldptr, oldsz);
	if (newsz > oldsz)
		memset(newptr + oldsz, 0, newsz - oldsz);

	dsa_free(MQPoolerDsaArea, olddp);

	return dp;
}