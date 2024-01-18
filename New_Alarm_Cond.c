/**************************************************************************************

Assignment 3 - New Alarm Cond.c

Stefano Giammarresi - 218040170
Danial Danial - 209411992 
Joshua Hanif - 217777009 
Cody Villareal - 215711377
Ricardo Santin-Andrade - 216981789

EECS 3221 Operating System Fundamentals
Dr. Jia Xu

December 5th 2023

DISCLAMER - This code was fully designed and written by the members of this group 
without any external sources and with FULL effort from everyone in the group.

************************************************************************************/

#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include "errors.h"
#include <sys/syscall.h>
#include <sys/types.h>
//#define DEBUG

/*
 * The "alarm" structure now contains the time_t (time since the
 * Epoch, in seconds) for each alarm, so that they can be
 * sorted. Storing the requested number of seconds would not be
 * enough, since the "alarm thread" cannot tell how long it has
 * been on the list.
 */

//Declare struct for the alarm list
typedef struct alarm_tag {
  struct alarm_tag *link;
  int seconds;
  time_t time; /* seconds from EPOCH */
  char message[128];
  int alarm_id;
  int group_id;
  int old_group;
  
  //Flag that is 1 if alarm is removed, 0 otherwise
  int alarm_removed;

  //Flag that is 1 if groupID is changed, 0 if unchanged 
  int alarm_changed;

  //Flag that is 1 if only the message is changed, 0 otherwise
  int message_changed;

  //Flag that is 1 if the message has been changed and needs to be printed 5 times, 0 otherwise
  int message_print;

  //Flag that is 1 if it is new to the display alarm thread, 0 otherwise
  int new;

} alarm_t;

//Declare struct for the thread list
typedef struct thread_group {
    struct thread_group *nextthread;
    pid_t pthrid;
    int thread_group;
    int seconds;

    //Each display thread will have its own list of alarms
    alarm_t *d_alarm_list;

    //Flag that is 1 if thread is active, 0 otherwise
    int active_thread;
} thread_grp;

int read_count = 0;

//Mutexes

//Semaphore used by all threads
pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER; 
//Semaphore only used by the reader and writer processes
pthread_mutex_t rw_mutex = PTHREAD_MUTEX_INITIALIZER; 

//Condition variables
pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t rw_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t disp_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t monitor_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t remove_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t change_cond = PTHREAD_COND_INITIALIZER;

alarm_t *cur_alarm;   //Reference to an already running alarm
alarm_t *alarm_list = NULL;  //Reference to the alarm list
alarm_t *change_alarm_list = NULL; //Reference to the change alarm list
alarm_t *old_alarm = NULL;  //Reference to the old alarm before it gets changed

thread_grp *thread_list = NULL; //Reference to the thread list

time_t current_alarm = 0;

/* Checks if an active thread with the given groupID exists */
int check_thread(int groupID)
{
  for(thread_grp *th = thread_list; th!=NULL; th=th->nextthread){
      if(th->thread_group == groupID && th->active_thread==1){
          return 1;
      }
  }
  return 0;
}

/*
 * Insert alarm entry on list, in order of alarm IDs.
 */
void alarm_insert (alarm_t *alarm) {
  int status;
  alarm_t **last, *next;
/*
 * LOCKING PROTOCOL:
 *
 * This routine requires that the caller have locked the
 * alarm_mutex!
 */
 last = &alarm_list;
 next = *last;

 while (next != NULL) {
   if (next->alarm_id >= alarm->alarm_id){
     alarm->link = next;

     *last = alarm;

     break;
   }
   last = &next->link;
   next = next->link;
 }
 /*
  * If we reached the end of the list, insert the new alarm
  * there. ("next" is NULL, and "last" points to the link
  * field of the last item, or to the list header.)
  */
  if (next == NULL) {
    *last = alarm;
    alarm->link = NULL;
  }
  
  /*DEBUG ONLY - print list*/
  #ifdef DEBUG
    printf ("[list: ");
  for (next = alarm_list; next != NULL; next = next->link)
    printf ("%d(%d)(%d)[\"%s\"] ", next->time, next->alarm_id, 
    next->time - time (NULL), next->message);
  printf ("]\n");
  #endif

  /*
  * Wake the alarm thread if it is not busy (that is, if
  * current_alarm is 0, signifying that it's waiting for
  * work), or if the new alarm comes before the one on
  * which the alarm thread is waiting.
  */
  if (current_alarm == 0 || alarm->time < current_alarm) {
      current_alarm = alarm->time;
      status = pthread_cond_signal (&alarm_cond);
    if (status != 0){
        printf("Status != 0\n");
        err_abort (status, "Signal cond");
    }
  }
}

/* Insert change alarm requests in order of arrival */
void change_alarm_insert (alarm_t *alarm) {
  int status;
  alarm_t **last, *next;
/*
 * LOCKING PROTOCOL:
 *
 * This routine requires that the caller have locked the
 * alarm_mutex!
 */
 last = &change_alarm_list;
 next = *last;

 while (next != NULL) {

   if (next->alarm_id >= alarm->alarm_id){
     alarm->link = next;

     *last = alarm;

     break;
   }
   last = &next->link;
   next = next->link;
 }
 /*
  * If we reached the end of the list, insert the new alarm
  * there. ("next" is NULL, and "last" points to the link
  * field of the last item, or to the list header.)
  */
  if (next == NULL) {
    *last = alarm;
    alarm->link = NULL;
  }
  /*DEBUG ONLY*/
  #ifdef DEBUG
    printf ("[Change Alarm list: ");
  for (next = change_alarm_list; next != NULL; next = next->link)
    printf ("%d(%d)(%d)[\"%s\"] ", next->time, next->alarm_id, 
    next->time - time (NULL), next->message);
  printf ("]\n");
  printf("Current time: %ld\n", time(NULL));
  #endif
}

/* Insert any newly created threads in order of arrival*/
void thread_insert (thread_grp *thread) 
{
    int status;
    thread_grp **tlast, *tnext;
    /*
    * LOCKING PROTOCOL:
    *
    * This routine requires that the caller have locked the
    * alarm_mutex!
    */
    tlast = &thread_list;
    tnext = *tlast;

    //Go to the end of the list
    while (tnext != NULL) 
    {
      tlast = &tnext->nextthread;
      tnext = tnext->nextthread;
    }
    /*
    * If we reached the end of the list, insert the new thread
    * there. ("next" is NULL, and "last" points to the link
    * field of the last item, or to the list header.)
    */
    if (tnext == NULL) {
      
      *tlast = thread;
      thread->nextthread = NULL;
    }
    #ifdef DEBUG
      printf ("[Thread list: ");
      for (tnext = thread_list; tnext != NULL; tnext = tnext->nextthread)
      {
        printf ("(%d) %d", tnext->thread_group, tnext->pthrid);
      }  
      printf ("]\n");
      printf("Current time: %ld\n", time(NULL));
    #endif
}

/* Runs when a process needs to get a specific thread alarm list */
alarm_t *get_alarm_list(int groupID)
{
  for(thread_grp *th = thread_list; th!=NULL; th=th->nextthread){
    
    if(th->thread_group==groupID && th->active_thread==1){
     
      return th->d_alarm_list;
    }
  }
  return NULL;
}

/* Display alarm thread */
void *display_alarm(void *arg){

    int status = 0;
    long x = (long) arg;

    int groupID = (int) x;

    alarm_t *display_alarm;

    //Initialize thread struct and allocate memory
    thread_grp *t;
    t = (thread_grp*)malloc (sizeof (thread_grp));
    if (t == NULL){
        errno_abort ("Could not allocate thread list");
    }

    //Declare thread ID by using syscall
    #ifdef SYS_gettid
        pid_t tidDisplay = syscall(SYS_gettid);
    #else
        #error "SYS_gettid unavailable on this system"
    #endif

    /*Save thread information to thread struct and add thread to thread list*/
    t->pthrid = tidDisplay;
    t->thread_group = groupID;
    t->active_thread = 1;
    thread_insert(t);

    //signal main thread that the thread struct is completed and added to thread list
    pthread_cond_signal(&disp_cond);

    //wait for thread alarm list to be populated
    pthread_cond_wait(&disp_cond,&alarm_mutex);
    alarm_t *d_alarm_list; 
    status = pthread_mutex_unlock(&alarm_mutex);

    //Loop continously, checks for alarms in the alarm list
    while(1)
    {   
     
      //Read process setup
     
      //Lock main mutex - protects read_count data
      //status = pthread_mutex_unlock(&alarm_mutex);
      status = pthread_mutex_lock(&alarm_mutex);
      if(status!=0)
      {
        err_abort (status, "Locking mutex");
      }
      read_count++;
      //check if reader process is the first reader
      if (read_count == 1)
      {
        //Lock read-write mutex
        status = pthread_mutex_lock(&rw_mutex);
        if(status!=0)
        {
          err_abort (status, "Cond wait");
        }
      }
      //unlock main mutex - allow other threads to modify read_count
      status = pthread_mutex_unlock(&alarm_mutex);
      if(status!=0)
      {
        err_abort (status, "Unlock mutex");
      }
      /* reading is performed */

      //Reading the alarms based on group ID
      d_alarm_list = get_alarm_list(groupID);

      //Check if an alarm is found and is not removed      
      if(d_alarm_list!=NULL)
      {
        //Check if an alarm has changed to a different group ID
        if(d_alarm_list->alarm_changed==1){
          printf("Display Thread %d Has Stopped Printing Message of Alarm(%d) at %lu: Changed Group(%d) %d %s\n", tidDisplay,d_alarm_list->alarm_id,time(NULL),d_alarm_list->group_id,d_alarm_list->seconds,d_alarm_list->message);
          pthread_cond_signal(&change_cond);
        }

        //Check if an alarm had its message changed
        if(d_alarm_list->message_changed==1){
            printf("Display Thread %d Starts to Print Changed Message Alarm(%d) at %lu: Group(%d) %d %s\n",tidDisplay,d_alarm_list->alarm_id,time(NULL),d_alarm_list->group_id,d_alarm_list->seconds,d_alarm_list->message);
            d_alarm_list = get_alarm_list(groupID);
            d_alarm_list->message_print = 1;
            d_alarm_list->message_changed = 0;
        }

        //Check if the message was changed and needs to print every 5 seconds
        if(d_alarm_list->message_print==1){
            printf("%s\n",d_alarm_list->message);
        }

        //Check if an alarm had now been changed to the same group ID as this thread
        if(d_alarm_list->new==1){
          printf("Display Thread %d Has Taken Over Printing Message of Alarm(%d) at %lu: Changed Group(%d) %d %s\n",tidDisplay,d_alarm_list->alarm_id,time(NULL),d_alarm_list->group_id,d_alarm_list->seconds,d_alarm_list->message);
          d_alarm_list->new=0;
        }

        //Check if alarm can be displayed
        if(d_alarm_list->alarm_removed==0 || d_alarm_list->alarm_changed==0) 
        {
          printf("Alarm (%d) Printed by Alarm Display Thread %d at %lu: Group(%d) %d %s\n",d_alarm_list->alarm_id,tidDisplay,time(NULL),d_alarm_list->group_id,d_alarm_list->seconds,d_alarm_list->message);
        }
        if(d_alarm_list->alarm_removed == 1){
          printf("Display Thread %d Has Stopped Printing Message of Alarm(%d) at %lu: Group(%d) %d %s\n",tidDisplay, d_alarm_list->alarm_id, time(NULL),d_alarm_list->group_id,d_alarm_list->seconds,d_alarm_list->message);

          //signal monitor thread to remove from display alarm list
          pthread_cond_signal(&remove_cond);

          //wait for list removal to be complete
          pthread_cond_wait(&remove_cond,&alarm_mutex);
          
          //unlock alarm_mutex after removal is done
          pthread_mutex_unlock(&alarm_mutex);
        }
      }
      //If the thread has no more alarms to display
      if(d_alarm_list == NULL)
      {
        //Remove thread from thread list
        printf("No More Alarms in Group(%d): Display Thread %d exiting at %lu\n",groupID,tidDisplay,time(NULL));
        t->active_thread = 0;

        /* Read process done */

        //Lock main mutex - protect read_count data
        pthread_mutex_lock(&alarm_mutex);
        if(status!=0)
        {
          err_abort (status, "Locking mutex");
        }
        read_count--;
        //Check if there are no more read processes
        if (read_count == 0)
        {
          //Unlock read/write mutex - allow other threads to modify alarm data
          pthread_mutex_unlock(&rw_mutex);
          if(status!=0)
          {
            err_abort (status, "unlock rw_mutex");
          }
        }
        //Unlock main mutex - protect read_count data
        pthread_mutex_unlock(&alarm_mutex);
        if(status!=0)
        {
          err_abort (status,  "unlock alarm_mutex");
        }

        //terminate thread
        return 0;
      }

      /* Read process done */

      //Lock main mutex - protect read_count data
      pthread_mutex_lock(&alarm_mutex);
      if(status!=0)
      {
        err_abort (status, "Locking mutex");
      }
      read_count--;
      //Check if there are no more read processes
      if (read_count == 0)
      {
        //Unlock read/write mutex - allow other threads to modify alarm data
        pthread_mutex_unlock(&rw_mutex);
        if(status!=0)
        {
          err_abort (status, "unlock rw_mutex");
        }
      }
      //Unlock main mutex - protect read_count data
      pthread_mutex_unlock(&alarm_mutex);
      if(status!=0)
      {
        err_abort (status, "unlock alarm_mutex");
      }
      
      //sleep for 5 seconds --> this will make the process repeat every 5 seconds
      sleep(5);   
    }
}

/*Helper method -- move the head of a thread's display alarm list to the next available alarm*/
void remove_alarm(int groupID)
{
  for(thread_grp *th = thread_list;th!=NULL;th=th->nextthread)
  {
    if(th->thread_group == groupID && th->active_thread == 1){
      th->d_alarm_list = th->d_alarm_list->link;
      break;
    }
  }
}

/*Alarms monitoring thread -- monitors for change requests and expired alarms*/
void *alarm_monitor(void *arg)
{
  alarm_t *change_alarm, *alarm;
  pthread_t thread;

  old_alarm = (alarm_t*)malloc (sizeof (alarm_t));
  if (old_alarm == NULL){
    errno_abort ("Could not allocate alarm");
  }

  //Get monitor thread ID
  #ifdef SYS_gettid
    pid_t tidMonitor = syscall(SYS_gettid);
  #else
    #error "SYS_gettid unavailable on this system"
  #endif

  while(1){
    int status = pthread_mutex_lock (&alarm_mutex);
    if (status != 0){
      err_abort (status, "Lock mutex");
    }
    if(change_alarm_list!=NULL){
    
      change_alarm = change_alarm_list;
      
        #ifdef DEBUG
        printf ("[Before change - Alarm list: ");
        for (alarm = alarm_list; alarm != NULL; alarm = alarm->link)
        {
            printf ("before - %d(%d)(%d)[\"%s\"] ", alarm->time, alarm->alarm_id, alarm->time - time (NULL), alarm->message);
        }
        printf ("]\n");

        #endif
      
      //Change the alarm if it is currently running
      if(cur_alarm!=NULL)
      {
        if(cur_alarm->alarm_id == change_alarm->alarm_id)
        {
          old_alarm->group_id = cur_alarm->group_id;
          strcpy(old_alarm->message,cur_alarm->message);

          cur_alarm->alarm_id = change_alarm->alarm_id;
          cur_alarm->group_id = change_alarm->group_id;
          cur_alarm->old_group = old_alarm->group_id;
          strcpy(cur_alarm->message,change_alarm->message);
          cur_alarm->seconds = change_alarm->seconds;
          cur_alarm->time = time (NULL) + change_alarm->seconds;

          //Determines if the group ID changes
          if(cur_alarm->group_id != old_alarm->group_id){
            cur_alarm->alarm_changed=1;
          }
          
          //Determine if only the message changes
          if(cur_alarm->group_id == old_alarm->group_id && strcmp(cur_alarm->message,old_alarm->message)!=0){
            cur_alarm->message_changed=1;
          }

          printf("Alarm Monitor Thread %d Has Changed Alarm(%d) at %lu: Group(%d) %d %s\n",tidMonitor, change_alarm->alarm_id, time(NULL), change_alarm->group_id, change_alarm->seconds, change_alarm->message);

        }
      }
      
      alarm = alarm_list;
      
      //Change the alarm if it is still in the alarm list
      while(alarm!=NULL)
      {
        if(change_alarm->alarm_id == alarm->alarm_id)
        {
            old_alarm->alarm_id=alarm->alarm_id;
            old_alarm->group_id=alarm->group_id;
            strcpy(old_alarm->message,alarm->message);
            old_alarm->seconds=alarm->seconds;
            old_alarm->time=alarm->time;

            alarm->alarm_id = change_alarm->alarm_id;
            alarm->group_id = change_alarm->group_id;
            alarm->old_group = old_alarm->group_id;
            strcpy(alarm->message,change_alarm->message);
            alarm->seconds = change_alarm->seconds;
            alarm->time = change_alarm->time;

            if(alarm->group_id != old_alarm->group_id){
              alarm->alarm_changed=1;
            }
            
            //Determine if only the message changes
            if(alarm->group_id == old_alarm->group_id && strcmp(alarm->message,old_alarm->message)!=0){
              alarm->message_changed=1;
            }
            
            printf("Alarm Monitor Thread %d Has Changed Alarm(%d) at %lu: Group(%d) %d %s\n",tidMonitor, change_alarm->alarm_id, time(NULL), change_alarm->group_id, change_alarm->seconds, change_alarm->message);
            break;
        }
        alarm=alarm->link;
      }
      //Check if the current alarm and the alarm list is not NULL so that it can be properly changed
      if(cur_alarm!=NULL)
      {
        if(alarm == NULL && cur_alarm->alarm_id!=change_alarm->alarm_id)
        {
          printf("Invalid Change Alarm Request(%d) at %d: Group(%d) %d %s\n", change_alarm->alarm_id, time(NULL), change_alarm->group_id, change_alarm->seconds, change_alarm->message);
        }
      }
      else if(cur_alarm==NULL)
      {
        printf("Invalid Change Alarm Request(%d) at %d: Group(%d) %d %s\n", change_alarm->alarm_id, time(NULL), change_alarm->group_id, change_alarm->seconds, change_alarm->message);
      }
      
      //Remove change alarm request from the change alarm list
      change_alarm_list = change_alarm->link;

      #ifdef DEBUG
        printf ("[After change - Alarm list: ");
        for (alarm = alarm_list; alarm != NULL; alarm = alarm->link)
        {
          printf ("after - %d(%d)(%d)[\"%s\"] ", alarm->time, alarm->alarm_id, alarm->time - time (NULL), alarm->message);
        }
        printf ("]\n");
      #endif
    }
    if(cur_alarm != NULL)
    {
      if(cur_alarm->alarm_changed == 1) //If monitor detects that the alarm's group ID has changed
      {
        
        //find old alarm in the display alarm list by using cur_alarm->alarm_id
        alarm_t *old_list = get_alarm_list(cur_alarm->old_group);

        old_list->alarm_changed=1;

        //Wait for alarms old display thread to detect the alarms changed groupID
        pthread_cond_wait(&change_cond,&alarm_mutex);
      
        //remove old alarm from the old display alarm list
        remove_alarm(cur_alarm->old_group);

        //Signal alarms old display thread that it has finished being removed
        pthread_cond_signal(&change_cond);

        //Create tmp alarm variable
        alarm_t *tmp;
        tmp = (alarm_t*)malloc (sizeof (alarm_t));
        if (tmp == NULL){
          errno_abort ("Could not allocate alarm");
        }
        
        /* 
        Check to see if the thread that is supposed to control the changed alarm group ID still exists. If not, then 
        create a new thread for displaying those alarms 
        */
        if(check_thread(cur_alarm->group_id)==0){
          long a = (long)cur_alarm->group_id;
          status = pthread_create (&thread, NULL, display_alarm, (void *)a);
          if (status != 0)
          {
              err_abort (status, "Create alarm thread");
          }
          //Wait for thread list to be populated
          pthread_cond_wait(&disp_cond,&alarm_mutex);
        }

        /*
        Add cur_alarm to the new display alarm list by using its new group ID. 
        It will be added to the head of that list since it is beginning to be processed
        */
        for(thread_grp *th=thread_list;th!=NULL;th=th->nextthread)
        {
          //Check if the thread is running or not  
          if(th->active_thread==1 && th->thread_group == cur_alarm->group_id)
          {
            //If the d_alarm_list in the alarm's new thread is empty
            if(th->d_alarm_list==NULL)
            {
              th->d_alarm_list = (alarm_t *)malloc(sizeof (alarm_t));
              if (th->d_alarm_list == NULL){
                errno_abort ("Could not allocate alarm");
              }
              th->d_alarm_list->alarm_id = cur_alarm->alarm_id;
              th->d_alarm_list->group_id = cur_alarm->group_id;
              
              th->d_alarm_list->time = cur_alarm->time;
              th->d_alarm_list->seconds = cur_alarm->seconds;
              //Indicate to alarm's new display thread that it has been changed to this one
              th->d_alarm_list->new = 1;
              strcpy(th->d_alarm_list->message,cur_alarm->message);
            }
            else{
              //If d_alarm_list for the alarm's new thread is already populated
              
              //Save the original first alarm of the new d_alarm_list in a tmp variable
              tmp->alarm_id = th->d_alarm_list->alarm_id;
              tmp->group_id = th->d_alarm_list->group_id;
              tmp->time = th->d_alarm_list->time;
              tmp->seconds = th->d_alarm_list->seconds;
              strcpy(tmp->message,th->d_alarm_list->message);
            
              //Current alarm is now first in the new list
              th->d_alarm_list->alarm_id = cur_alarm->alarm_id;
              th->d_alarm_list->group_id = cur_alarm->group_id;
              th->d_alarm_list->time = cur_alarm->time;
              th->d_alarm_list->seconds = cur_alarm->seconds;
              strcpy(th->d_alarm_list->message, cur_alarm->message);
              //Set 'new' flag to 1 indicating to the new display thread that it is a new alarm
              th->d_alarm_list->new = 1;
             
              //The original first alarm in the new d_alarm_list is now shifted to be after the newly changed alarm
              
              //Check if there is more than 1 alarm in the new d_alarm_list
              if(th->d_alarm_list->link==NULL)
              {
                th->d_alarm_list->link = tmp;
              }
              else
              {
                th->d_alarm_list->link->alarm_id = tmp->alarm_id;
                th->d_alarm_list->link->group_id = tmp->group_id;
                th->d_alarm_list->link->time = tmp->time;
                th->d_alarm_list->link->seconds = tmp->seconds;
                strcpy(th->d_alarm_list->link->message, tmp->message);
              }
            }
            break;
          } 
        }
        //Signal any newly created display alarm threads that thread alarm list is populated
        pthread_cond_signal(&disp_cond);

        /*
        update change_flag to 0 for cur_alarm in the MAIN alarm list, this indicates to the 
        main alarm list that the change is done prevents the change procedure from looping forever
        */
        cur_alarm->alarm_changed = 0;

      }
      if(cur_alarm->message_changed==1){ //If monitor detects that the alarms message has changed but not the groupID

        //Find alarm and change its message
        for(thread_grp *th=thread_list; th!=NULL; th=th->nextthread)
        {
            if(th->active_thread == 1 && th->thread_group == cur_alarm->group_id)
            {
                th->d_alarm_list->seconds = cur_alarm->seconds;
                strcpy(th->d_alarm_list->message,cur_alarm->message);
                th->d_alarm_list->message_changed=1;
            }
        }
        cur_alarm->message_changed=0;

      }
      if(cur_alarm->alarm_removed == 1){
        int cur_id = cur_alarm->group_id;
        alarm_t *monitor_alarm = get_alarm_list(cur_alarm->group_id);
        
        //Flag the display alarm as removed
        monitor_alarm->alarm_removed = 1;

        printf("Alarm Monitor Thread %d Has Removed Alarm(%d) at %lu: Group(%d) %d %s\n",tidMonitor,monitor_alarm->alarm_id,time(NULL),monitor_alarm->group_id,monitor_alarm->seconds,monitor_alarm->message);
        
        //Set cur_alarm to NULL
        cur_alarm = NULL;

        pthread_cond_signal(&monitor_cond);
        
        //Wait for display alarm thread to read the removed alarm
        pthread_cond_wait(&remove_cond,&alarm_mutex);

        //Move display alarm list header
        remove_alarm(cur_id);

        //Signal to display thread that the alarm has been removed
        pthread_cond_signal(&remove_cond);

      }
    }
    status = pthread_mutex_unlock (&alarm_mutex);
    if (status != 0){
      err_abort (status, "Unlock mutex");
    }
  }
  
}

/*
 * The alarm thread's start routine.
 */
void *alarm_thread (void *arg)
{
  alarm_t *alarm;
  struct timespec cond_time;
  time_t now;
  int status, expired;

  int count = 0;

  /*
  * Loop forever, processing commands. The alarm thread will
  * be disintegrated when the process exits. Lock the mutex
  * at the start -- it will be unlocked during condition
  * waits, so the main thread can insert alarms.
  */
  status = pthread_mutex_lock (&alarm_mutex);
  if (status != 0){
    err_abort (status, "Lock mutex");
  }
  while (1) {
    /*
    * If the alarm list is empty, wait until an alarm is
    * added. Setting current_alarm to 0 informs the insert
    * routine that the thread is not busy.
    */
    current_alarm = 0;
    while (alarm_list == NULL) {
      status = pthread_cond_wait (&alarm_cond, &alarm_mutex);
      
      if (status != 0){
        err_abort (status, "Wait on cond");
      }
    }
    //Locks mutex
    alarm = alarm_list;

    //Save the current alarm for the alarm monitor and display thread
    cur_alarm = alarm;
   
    alarm_list = alarm->link;
    count++;

    now = time (NULL);
    expired = 0;
    if (cur_alarm->time > now) {
#ifdef DEBUG
      printf ("[waiting: %d(%d)\"%s\"]\n", alarm->time,
        cur_alarm->time - time (NULL), alarm->message);
#endif
      cond_time.tv_sec = cur_alarm->time;
      cond_time.tv_nsec = 0;
      current_alarm = cur_alarm->time;
      while (current_alarm == cur_alarm->time) {
        status = pthread_cond_timedwait (&alarm_cond, &alarm_mutex, &cond_time);
        if (status == ETIMEDOUT) {
          expired = 1;
          break;
        }
        if (status != 0)
          err_abort (status, "Cond timedwait");
      }
      if (!expired)
      {
        alarm_insert (cur_alarm);
      }
        
  } else{
      expired = 1;
    }
    if (expired) 
    {
      printf ("(%d) %s\n", cur_alarm->seconds, cur_alarm->message); 
      cur_alarm->alarm_removed=1;
      free (alarm);

      //Wait for monitor to be done with cur_alarm
      pthread_cond_wait(&monitor_cond, &alarm_mutex);
    }
  }
}

/* Helper method --- check if the input alarm ID is a genuine ID not used by another alarm */
int isGenuine(int alarmID)
{
    //First alarm ID will always be genuine
    if(cur_alarm==NULL){
        return 1;
    }
    //Check the alarm that is running
    if(cur_alarm->alarm_id == alarmID){
        return 0;
    }
    if(alarm_list==NULL)
    {
        return 1;
    }
    //Check the alarms in the alarm list
    for(alarm_t *a = alarm_list;a!=NULL;a=a->link){
        if(a->alarm_id == alarmID){
            return 0;
        }
    }
    //If no identical alarm IDs are found, return 1
    return 1;

}

int requestValidation(char line[]){
    char keyID[512] = "Fail";
    char ggID[512] = "Fail";
    char keyword[50] = "Fail";
    char group[512] = "FAIL";
    int alarmID =-1, groupID =-1, alarmTime=-1;
    int validKeyword;
 
    sscanf(line, "%s %s %d", keyID, ggID, &alarmTime);
    sscanf(keyID, "%[^\(] (%d[^)]", keyword, &alarmID);
    sscanf(ggID, "%[^\(] (%d[^)]", group, &groupID);
    
    if (strcmp(keyword, "Start_Alarm")!=0 && strcmp(keyword, "Change_Alarm")!=0){
      printf("Invalid Keyword\n");
      return 0;
    }
    else if(strcmp(group, "Group")!=0){
      printf("Invalid Group Keyword\n");
      return 0;
    }
    else if(alarmTime<=0){
      printf("invalid alarm Time\n");
      return 0;
    }
    else if(groupID<0 || alarmID<0)
    {
      printf("groupID or alarmID is invalid\n");
      return 0;
    }
    else {
      return 1;
    }
}

int main (int argc, char *argv[]){
  int status;
  char line[1024];
  char kwid[1024]; //for splicing keyword and id
  char keyword[50]; //Start_Alarm vs Change_Alarm
  char group[200];
  char grp[100];
  int alarm_id; //int up to 2147483647
  int group_id;
  char message[128];
  alarm_t *alarm;
  pthread_t thread;

  #ifdef SYS_gettid
    pid_t tidMain = syscall(SYS_gettid);
  #else
    #error "SYS_gettid unavailable on this system"
  #endif

  //Create alarm thread
  status = pthread_create (
    &thread, NULL, alarm_thread, NULL);
  if (status != 0)
    err_abort (status, "Create alarm thread");

  //Create alarm monitor thread
  status = pthread_create (
    &thread, NULL, alarm_monitor, NULL);
  if (status != 0)
    err_abort (status, "Create alarm monitor thread");

  while (1) {
    printf ("Alarm> ");
    if (fgets (line, sizeof (line), stdin) == NULL){ 
      exit (0);
      }
    if (strlen (line) <= 1){
      continue;
    }
    alarm = (alarm_t*)malloc (sizeof (alarm_t));
    if (alarm == NULL){
      errno_abort ("Allocate alarm");
    }

    /*
    * Parse input line into seconds (%d) and a message
    * (%128^\n]), consisting of up to 128 characters
    * separated from the seconds by whitespace.
    */
    if (sscanf(line, "%s %s %d %128[^\n]",kwid, group, &alarm->seconds, alarm->message) < 4) {
      fprintf (stderr, "Bad command\n");
      free(alarm);
    } 
    else if(requestValidation(line)==0){ //Check if the input line is valid
      printf("Input line is incorrect\n");
      free(alarm);
    }
    else {
      //Lock mutex before adding alarms
      status = pthread_mutex_lock (&alarm_mutex);
       if (status != 0){
        err_abort (status, "Lock mutex");
      }
      //Parse the keyword, alarmID and groupID
      sscanf(kwid, "%[^\(] (%d[^)]", keyword, &alarm_id);
      sscanf(group, "%[^\(] (%d[^)]", grp, &group_id);

      alarm->time = time (NULL) + alarm->seconds;
      alarm->alarm_id = alarm_id;
      alarm->group_id = group_id;
      
      //Save groupID into a placeholder variable to be assigned to a display alarm thread
      long a = (long) group_id;

      if(strcmp(keyword, "Start_Alarm")==0 && isGenuine(alarm->alarm_id)==1)
      {
            /*
            * Insert the new alarm into the list of alarms,
            * sorted by expiration time.
            */

            //Lock rw_mutex
            pthread_mutex_lock(&rw_mutex);

            //Insert alarm to alarm_list
            alarm_insert (alarm);
    
            //Unlock rw_mutex
            pthread_mutex_unlock(&rw_mutex);
       

            //Check if group ID is already assigned to an existing thread
            if(check_thread(alarm->group_id) == 0)
            {
                //Create new display alarm thread
                status = pthread_create (&thread, NULL, display_alarm, (void *)a);
                if (status != 0)
                {
                    err_abort (status, "Create alarm thread");
                }
                //Wait for thread list to be populated
                pthread_cond_wait(&disp_cond,&alarm_mutex);
                pid_t dispthreadID;
                //Get the newly created thread ID
                for(thread_grp *th = thread_list;th!=NULL;th=th->nextthread){
                  if(th->thread_group == alarm->group_id && th->active_thread==1){
                    dispthreadID=th->pthrid;
                    break;
                  }
                }
                printf("Main Thread Created New Display Alarm Thread %d For Alarm(%d) at %lu: Group(%d) %d %s\n",dispthreadID,alarm->alarm_id,time(NULL),alarm->group_id,alarm->seconds,alarm->message);
            }
            
            //Prepare alarm to add to threads display alarm and the traversal of the display alarm list
            alarm_t **last2, *next2;

            alarm_t *dispalarm;
            dispalarm = (alarm_t*)malloc (sizeof (alarm_t));
            if (dispalarm == NULL){
                errno_abort ("Allocate alarm");
            }
            //Copy alarm data to different alarm struct for the thread alarm list
            dispalarm->alarm_id = alarm->alarm_id;
            dispalarm->group_id = alarm->group_id;
            strcpy(dispalarm->message,alarm->message);
            dispalarm->seconds = alarm->seconds;
          
            //Find thread that contains the alarm group ID and add that alarm to the thread alarm list
            for(thread_grp *th = thread_list; th!=NULL; th=th->nextthread)
            {
                //Find the thread that controls the alarm group ID
                if(th->thread_group == alarm->group_id && th->active_thread==1)
                {
                    //Add that alarm to the threads specific alarm list
                    last2 = &th->d_alarm_list;
                    next2 = *last2;
                    while(next2!=NULL)
                    {
                        if (next2->alarm_id >= dispalarm->alarm_id)
                        {
                            dispalarm->link = next2;
                            *last2 = dispalarm;
                            break;
                        }
                        last2 = &next2->link;
                        next2 = next2->link;
                    }
                    if(next2==NULL)
                    {
                        *last2 = dispalarm;
                        dispalarm->link = NULL;
                    }
                }
            }
            printf("Main Thread %d Assigned to Display Alarm(%d) at %lu: Group(%d) %d %s\n",tidMain,alarm->alarm_id,time(NULL),alarm->group_id,alarm->seconds,alarm->message);
            
            //Signal display alarm thread that thread alarm list is populated
            pthread_cond_signal(&disp_cond);

            printf("Alarm(%d) Inserted by Main Thread %d Into Alarm List at %ld: Group(%d) %d %s\n",alarm->alarm_id, tidMain, time(NULL), alarm->group_id, alarm->seconds, alarm->message);

            //Unlock mutex
            status = pthread_mutex_unlock (&alarm_mutex);
            if (status != 0)
            {
                err_abort (status, "Unlock mutex");
            }
        }
        else if(strcmp(keyword, "Change_Alarm")==0)
        {   
          //Run 'change_alarm_insert()' to add the change alarm request to a list
          change_alarm_insert(alarm);
          
          printf("Change Alarm Request (%d) Inserted by Main Thread %d into Change Alarm List at %lu: Group(%d) %d %s\n", alarm->alarm_id, tidMain, time(NULL), alarm->group_id, alarm->seconds, alarm->message);

          //Unlock mutex
          status = pthread_mutex_unlock (&alarm_mutex);
          if (status != 0){
          err_abort (status, "Unlock mutex");
          }

        }
        else{
            printf("Alarm ID already exists\n");
            free(alarm);
            pthread_mutex_unlock(&alarm_mutex);
        }
    }
  }
}
