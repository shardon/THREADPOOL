/*
** threadpool.c
** This file contains implementation of a threadpool.
** Created by Ricky Weng
*/

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>

#include "threadpool.h"

typedef struct taskNode_st taskNode ;
typedef struct taskHead_st taskHead ;
typedef struct workerNode_st workerNode ;
typedef struct workerHead_st workerHead ;

typedef enum { PRUN, PEXIT } state_t ;

struct taskNode_st
{
	task_fn	task_func ;
	clean_fn clean_func ;
	void *task_arg ;
	void *clean_arg ;
	taskNode *next ;
} ;
struct taskHead_st
{
	taskNode *head ;
	taskNode *tail ;
	taskNode *recycle ;
	int cap ;
	int max_cap ;
	int avail ;
} ;
struct workerNode_st
{
	pthread_t pid ;
	workerNode *next ; 
} ;
struct workerHead_st
{
	workerNode *head ; 
	workerNode *recycle ; 
	int min ;
	int live ;
	int max ;
} ;
typedef struct _threadpool_st
{
	struct timeval  created ;	
	workerHead		theWorker ;
	taskHead		theTask ;
	pthread_mutex_t mutex ;
	pthread_cond_t  job_posted ;
	pthread_cond_t  job_taken ;
	state_t			state ;
} _threadpool ;

static void *doWork(void * ) ;
static void addWorker(_threadpool *) ;
static void delWorker( workerHead *,pthread_t) ;
static int addTask(taskHead *,task_fn,void *,clean_fn,void *) ;
static void getTask(taskHead *,task_fn *,void **,clean_fn*,void **) ;
static void freeTask(taskHead *) ;
static void freeWorker(workerHead *) ;

threadpool create_threadpool(int min,int max,int qsize)
{
	_threadpool *pool ;
	int i ;

	if ( min<=0 || min>max || max > MAXT_IN_POOL ) return NULL ;

	pool = (_threadpool *) malloc(sizeof(_threadpool)) ;
	if (pool == NULL) return NULL ;

	pthread_mutex_init(&(pool->mutex), NULL) ;
	pthread_cond_init(&(pool->job_posted), NULL) ;
	pthread_cond_init(&(pool->job_taken), NULL) ;
	pool->theWorker.min = min ;
	pool->theWorker.max = max ;
	pool->theWorker.live = 0 ;
	pool->theWorker.head = NULL ;
	pool->theWorker.recycle = NULL ;
	pool->theTask.max_cap = qsize>0?qsize:max;
	pool->theTask.cap = 0 ;
	pool->theTask.avail = 0 ;
	pool->theTask.head = NULL ;
	pool->theTask.tail = NULL ;
	pool->theTask.recycle = NULL ;
	pool->state = PRUN ;
	gettimeofday(&pool->created, NULL) ;

	for ( i=0 ; i<min ; i++ ) addWorker(pool) ;
	return (threadpool)pool ;
}
static void addWorker( _threadpool *pool)
{
	register workerHead *theWorker = &pool->theWorker ;
	register workerNode *temp ;
	pthread_t tid ;

	pthread_create(&tid, NULL, doWork, (void *) pool) ;
	pthread_detach(tid) ;  

	if(theWorker->recycle == NULL )
	{
		theWorker->recycle = (workerNode *)malloc(sizeof(workerNode)) ;
		if(theWorker->recycle == NULL) exit(EXIT_FAILURE) ;
		theWorker->recycle->next = NULL ;
	}
	temp = theWorker->recycle ;
	theWorker->recycle = temp->next ;

	temp->pid = tid ;
	temp->next = theWorker->head ;
	theWorker->head = temp ;
	theWorker->live ++ ;
}
static void delWorker(workerHead *theWorker,pthread_t tid)
{
	register workerNode *p1 = NULL ;
	register workerNode *p2 = NULL ;

	if( (p1 = p2 = theWorker->head) == NULL ) return ;
	if( p1->pid == tid ) theWorker->head = theWorker->head->next ;
	else
	{
		while((p1=p1->next))
		{
			if(p1->pid == tid )
			{
				p2->next = p1->next ;
				break ;
			}
			p2 = p1 ;
		}
	}
	if(p1)
	{
		p1->next = theWorker->recycle ;
		theWorker->recycle = p1 ;
		if( theWorker->live > 0 ) theWorker->live -- ;
	}
}
static void *doWork(void *owning_pool)
{
	_threadpool *pool = (_threadpool *) owning_pool ;
  
	state_t		mystate = PRUN ;
	task_fn		mytask = NULL ;
	clean_fn	mycleaner = NULL ;
	void		*myarg = NULL ;
	void		*mycleanarg = NULL ;

	pthread_setcanceltype( PTHREAD_CANCEL_ASYNCHRONOUS , NULL ) ;

	pthread_cleanup_push( (clean_fn)pthread_mutex_unlock, (void *) &pool->mutex ) ;

	for( ; ; )
	{
		pthread_mutex_lock(&pool->mutex) ;
		while( pool->theTask.head == NULL)
		{
			if( pool->theWorker.live > pool->theWorker.min || pool->state == PEXIT )
			{
				mystate = PEXIT ;
				break ;
			}
			pthread_cond_wait(&pool->job_posted, &pool->mutex) ;
		}

		if( mystate == PEXIT)
		{
			delWorker(&pool->theWorker,pthread_self()) ;
			break ;
		}
		
		getTask(&pool->theTask, &mytask, &myarg, &mycleaner, &mycleanarg) ;
		pthread_cond_signal(&pool->job_taken) ;

		pthread_mutex_unlock(&pool->mutex) ;

		if( mytask )
		{
			if( mycleaner )
			{
				pthread_cleanup_push(mycleaner,mycleanarg) ;
				mytask(myarg) ;
				pthread_cleanup_pop(1) ;

			}else 
			{
				mytask(myarg) ;
			}
			mytask=myarg=mycleaner=mycleanarg=NULL ;
		}
	}

	pthread_cleanup_pop(1) ;

	return NULL ;
}  
int
dispatch(threadpool from_me,task_fn task_here,void *arg,clean_fn cleaner,void* c_arg)
{
	_threadpool *pool = (_threadpool *) from_me ;
	int rank = 0 ; char isAdd= 1 ;

	if(pool != (_threadpool *) arg)
	{
		pthread_cleanup_push((clean_fn)pthread_mutex_unlock,(void *) &pool->mutex) ;
  
		pthread_mutex_lock(&pool->mutex) ;

		while(pool->theTask.recycle == NULL && pool->theTask.cap >= pool->theTask.max_cap)
		{
			if(pool->theWorker.live < pool->theWorker.max) addWorker(pool) ;
			else { isAdd= 0; break; }
			pthread_cond_signal(&pool->job_posted) ;
			pthread_cond_wait(&pool->job_taken,&pool->mutex) ;
		}
		
		if(isAdd)
		{
			rank = addTask(&pool->theTask,task_here,arg,cleaner,c_arg) ;
			if(rank> 1 && pool->theWorker.live < pool->theWorker.max) addWorker(pool) ;
		}
		
		//pthread_cond_signal(&pool->job_posted) ;
		pthread_cond_broadcast(&pool->job_posted);
  
		pthread_mutex_unlock(&pool->mutex) ;
		
		pthread_cleanup_pop(0) ;
	}
	return rank ;
}
void destroy_threadpool(threadpool destroyme,int flag) 
{
	_threadpool *pool = (_threadpool *) destroyme ;
	workerNode *wp ; 
	int oldtype ;

	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldtype) ;
	pthread_cleanup_push((clean_fn)pthread_mutex_unlock, (void *) &pool->mutex) ;  

	if(flag)
	{
		for( wp=pool->theWorker.head ; wp ; wp=wp->next ) pthread_cancel(wp->pid) ;
	
	}else
	{
		pthread_mutex_lock(&pool->mutex) ;
		pool->state = PEXIT ;
		pthread_mutex_unlock(&pool->mutex) ;

		while ( pool->theWorker.live )
		{
			pthread_cond_broadcast(&pool->job_posted);
			usleep(100);
		}
	}
	freeWorker(&pool->theWorker) ;
	freeTask(& pool->theTask) ;

	pthread_cleanup_pop(0) ;
  
	pthread_mutex_destroy(&pool->mutex) ;
	pthread_cond_destroy(&pool->job_posted) ;
	pthread_cond_destroy(&pool->job_taken) ;

	free(pool) ;
}
static void freeWorker(workerHead *theWorker)
{
	workerNode *temp ;
	
	while( theWorker->head || (theWorker->head = theWorker->recycle) )
	{
		if( theWorker->head == theWorker->recycle ) theWorker->recycle=NULL ;
		temp = theWorker->head ;
		theWorker->head = theWorker->head->next ;
		free(temp) ;
	}
}
static void freeTask(taskHead *theTask)
{
	taskNode *temp ;

	while( theTask->head || (theTask->head = theTask->recycle) )
	{
		if(theTask->head == theTask->recycle) theTask->recycle=NULL ;
		temp = theTask->head ;
		theTask->head = theTask->head->next ;
		free(temp) ;
	}
}
static int addTask(taskHead *theTask,task_fn f1,void *a1,clean_fn f2,void *a2)
{
	register taskNode * temp ;
		
	if(theTask->recycle == NULL)
	{
	    theTask->recycle = (taskNode *) malloc (sizeof(taskNode)) ;
		if(theTask->recycle == NULL) exit(EXIT_FAILURE) ;
		theTask->recycle->next = NULL ;
		theTask->cap ++ ;
	}
	
	temp = theTask->recycle ;
	theTask->recycle = temp->next ;
	
	temp->task_func = f1 ;
	temp->task_arg = a1 ;
	temp->clean_func = f2 ;
	temp->clean_arg = a2 ;
	temp->next=NULL ;

	if(theTask->tail)
	{
		theTask->tail->next = temp ;
		theTask->tail = temp ;

	}else theTask->head = theTask->tail = temp ;

	return ++ theTask->avail;
	
}
static void getTask(taskHead *theTask,task_fn *f1,void **a1,clean_fn *f2,void **a2)
{
	register taskNode * temp ;
		
	if((temp = theTask->head)==NULL) return ;
	if(theTask->head == theTask->tail)
	{
		theTask->head = theTask->tail = NULL ;
	}
	else theTask->head = temp->next ;
	
	*f1 = temp->task_func ;
	*a1 = temp->task_arg ;
	*f2 = temp->clean_func ;
	*a2 = temp->clean_arg ;

	temp->next=theTask->recycle ;
	theTask->recycle=temp ;
	theTask->avail -- ;
}
