#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <pthread.h>
#include <ctype.h>


#define	QLEN			5
#define	BUFSIZE			4096
#define QBUF			128
#define ADMIN		"QS|ADMIN\r\n"
#define JOIN		"QS|JOIN\r\n"
#define FULL		"QS|FULL\r\n"
#define WAIT		"WAIT\r\n"
#define CRLF        "\r\n"

void* run_quiz(void* ssock);

int clients = 0;

int passivesock( char *service, char *protocol, int qlen, int *rport );
int process_answer(char ans, char right_ans);
int search_group(char* groupname);

typedef struct Player 
{
	char	name[50];
    int     score;
    int     socket;
} Player;

typedef struct Quiz 
{
	int number_of_questions;
    char questions[128][128];
    char correct_answers[128];

} Quiz;

typedef struct Group 
{
	Quiz quiz;
    char            name[50];
	char			topic[50];
    int             max_size;
    Player          players[50];
    int             admin_sock;
    int             curr_size;
	pthread_t quiz_thread;

	/*
	pthread_mutex_t mutexJoin;
	pthread_mutex_t mutexInQuiz;
	pthread_cond_t condGroup;
	pthread_cond_t condInQuiz;
	*/

} Group;

Quiz get_quiz(char *quiztext);
void remove_group(int admin_sock);

Group list[32];

fd_set afds_main;
fd_set rfds_main;
int	   nfds_main;
char buf[BUFSIZE];

int group_index = 0;

int
main( int argc, char *argv[] )
{
	char			buf[BUFSIZE];
	char			*service;
	struct sockaddr_in	fsin;
	int			msock;
	int			ssock;
	int			alen;
	int			fd;
	int			rport = 0;
	int			cc;
	
	// Same arguments as usual
	switch (argc) 
	{
		case	1:
			// No args? let the OS choose a port and tell the user
			rport = 1;
			break;
		case	2:
			// User provides a port? then use it
			service = argv[1];
			break;
		default:
			fprintf( stderr, "usage: server [port]\n" );
			exit(-1);
	}

	// Create the main socket as usual
	// This is the socket for accepting new clients
	msock = passivesock( service, "tcp", QLEN, &rport );
	if (rport)
	{
		//	Tell the user the selected port
		printf( "server: port %d\n", rport );	
		fflush( stdout );
	}

	// Now we begin the set up for using select
	
	// nfds is the largest monitored socket + 1
	// there is only one socket, msock, so nfds is msock +1
	// Set the max file descriptor being monitored
	nfds_main = msock+1;

	// the variable afds is the fd_set of sockets that we want monitored for
	// a read activity
	// We initialize it to empty
	FD_ZERO(&afds_main);
	
	// Then we put msock in the set
	FD_SET( msock, &afds_main);

	// Now start the loop
	for (;;)
	{

		// Since select overwrites the fd_set that we send it, 
		// we copy afds into another variable, rfds
		// Reset the file descriptors you are interested in
		memcpy((char *)&rfds_main, (char *)&afds_main, sizeof(rfds_main));

		// Only waiting for sockets who are ready to read
		//  - this includes new clients arriving
		//  - this also includes the client closed the socket event
		// We pass null for the write event and exception event fd_sets
		// we pass null for the timer, because we don't want to wake early
		if (select(nfds_main, &rfds_main, (fd_set *)0, (fd_set *)0,
				(struct timeval *)0) < 0)
		{
			fprintf( stderr, "server select: %s\n", strerror(errno) );
			exit(-1);
		}

		// Since we've reached here it means one or more of our sockets has something
		// that is ready to read
		// So now we have to check all the sockets in the rfds set which select uses
		// to return a;; the sockets that are ready

		// If the main socket is ready - it means a new client has arrived
		// It must be checked separately, because the action is different
		if (FD_ISSET( msock, &rfds_main)) 
		{
			int	ssock;
			alen = sizeof(fsin);
			ssock = accept( msock, (struct sockaddr *)&fsin, &alen );
			if (ssock < 0)
			{
				fprintf( stderr, "accept: %s\n", strerror(errno) );
				exit(-1);
			}

			FD_SET( ssock, &afds_main);

			if ( ssock+1 > nfds_main )
				nfds_main = ssock+1;
			
			char opengroups[BUFSIZ];
			opengroups[0] = '\0';
			strcpy(opengroups, "OPENGROUPS");
			//OPENGROUPS|topic|name|size|curr
			int g;
			if (group_index>0)
			{
				for(g = 0; g < group_index; g++)
				{
					char group_info[BUFSIZ];
					sprintf(group_info, "|%s|%s|%d|%d", list[g].topic, list[g].name, list[g].max_size, list[g].curr_size);
					strcat(opengroups, group_info);
				}
			}
			strcat(opengroups, CRLF);
			write(ssock, opengroups, strlen(opengroups));
		}

		// Now check all the regular sockets
		for ( fd = 0; fd < nfds_main; fd++ )
		{		
			char msg[BUFSIZE];

			if (fd != msock && FD_ISSET(fd, &rfds_main))
			{
				struct Player p;
				p.score = 0;
				p.socket = fd;
				int creator_index;			//index to track id of group created
				int join_index;			//index to track id of group joined
					
				if ( (cc = read( fd, buf, BUFSIZE )) <= 0 )
				{
					printf( "The client has gone.\n" );
					(void) close(fd);
					
					FD_CLR( fd, &afds_main );

					if ( nfds_main == fd+1 )
						nfds_main--;
				}
				else
				{
					clients++;

					if(!strncmp("GETOPENGROUPS", buf, 13))
					{
						printf("Getting open groups...\n");
						char opengroups[BUFSIZ];
						opengroups[0] = '\0';
						strcpy(opengroups, "OPENGROUPS");
						//OPENGROUPS|topic|name|size|curr
						int g;
						if (group_index>0)
						{
							for(g = 0; g < group_index; g++)
							{
								char group_info[BUFSIZ];
								sprintf(group_info, "|%s|%s|%d|%d", list[g].topic, list[g].name, list[g].max_size, list[g].curr_size);
								printf("topic %s\nname %s\nmax %d\ncurr %d\n", list[g].topic, list[g].name, list[g].max_size, list[g].curr_size);
								strcat(opengroups, group_info);
							}
						}
						strcat(opengroups, CRLF);
						write(fd, opengroups, strlen(opengroups));
					}
					else if (!strncmp("GROUP", buf, 5))
					{
						int group_size;
						strtok(buf, "|");
						char *topic = strtok(NULL, "|");
						char *groupname = strtok(NULL, "|");
						char *quiznum = strtok(NULL, "|");

						if (atoi(quiznum) > 1)
						{
							group_size = atoi(quiznum);
							strcpy(msg ,"Created the group");
						}
						else
						{
							write(fd, "BAD", strlen("BAD"));
							continue;
						}
						Group group;
						group.name[0] = '\0';
						strcpy(group.name, "");
						strcat(group.name, groupname);

						group.topic[0] = '\0';
						strcpy(group.topic, "");
						strcat(group.topic, topic);

						group.max_size = atoi(quiznum);
						group.curr_size = 1;
						group.admin_sock = fd;

						p.name[0] = '\0';
						strcpy(p.name, "");
						strcat(p.name, "GroupAdmin");
						group.players[0] = p;

						list[group_index] = group;
						creator_index = group_index;
						group_index++;

						printf("Created Group %d: %s\nCurrent players: %d\n%d maximum players\nadmin socket: %d\n", group_index, group.name,group.curr_size,group.max_size, fd);
						write(fd, "SENDQUIZ", strlen("SENDQUIZ"));
					}

					else if(!strncmp("JOIN", buf, 4))
					{
						strtok(buf, "|");
						char *groupname = strtok(NULL, "|");
						char *name  = strtok(NULL, "|");
						printf("subtring: [%s]\n", buf);
						int index = search_group(groupname);
						if(index >= 0)
						{
							printf("Group exists\n");
							p.name[0] = '\0';
							strcpy(p.name, "");
							strcat(p.name, strtok(name, CRLF));

							printf("Client \"%s\" entered with socket: %d\n",p.name, p.socket);
							if(list[index].curr_size >= list[index].max_size)
							{
								printf("FULL");
								write(fd, "BAD", strlen("BAD"));
								continue;
							}
							else if(list[index].curr_size < list[index].max_size - 1)
							{
								write(fd, "OK", strlen("OK"));
								join_index = index;
								printf("\"%s\" joined the group with id: %d\n",p.name, list[index].curr_size);
								strcpy(msg , "Joined the group");

								/*
								FD_CLR( fd, &afds_main );
								if ( nfds == fd+1 )
									nfds--;
								*/

								//pthread_mutex_lock(&list[index].mutexJoin);
								//write(fd, WAIT, strlen(WAIT));
								list[index].players[list[index].curr_size] = p;
								list[index].curr_size++;
								/*
								if (list[index].curr_size != list[index].max_size && !list[index].ended)
									pthread_cond_wait(&list[index].condGroup, &list[index].mutexJoin);
								else
									pthread_cond_broadcast(&list[index].condGroup);

								pthread_mutex_unlock(&list[index].mutexJoin);
								*/
					
							}
							else if(list[index].curr_size == list[index].max_size - 1)
							{
								write(fd, "OK", strlen("OK"));
								//write(fd, WAIT, strlen(WAIT));
								list[index].players[list[index].curr_size] = p;
								list[index].curr_size++;

								if(pthread_create(&list[index].quiz_thread, NULL, run_quiz, (void*)index) < 0)
								{
									printf(stderr, "ERROR: %s\n", strerror(errno));
									exit(-1);
								}
							}
						}
						else
						{
							printf("Group doesn't exist\n");
							write(fd, "BAD", strlen("BAD"));
						}
					}
					else if(!strncmp("QUIZ", buf, 4))
					{
						strtok(buf, "|");
						char *quizsize = strtok(NULL, "|");
						char *quiztext  = strtok(NULL, "|");


						printf("accessing quiz at: %d\n", creator_index) ;
						fflush(stdout);
						list[creator_index].quiz = get_quiz(quiztext);

						write(fd, "OK", strlen("OK"));
					}
					else if(!strncmp("LEAVE", buf, 5) && fd != list[creator_index].admin_sock)
					{
						printf("Player left the group\n");
						
						//remove and shift player array
						printf("Before\n");
						for(int j = 0; j<list[join_index].curr_size;j++)
						{
							printf("%s: %d\n", list[join_index].players[j].name, list[join_index].players[j].socket);
						}

						for(int i = 0; i<32; i++)
    					{
        					if(list[join_index].players[i].socket == p.socket)
        					{
            					for (int k = i; k<31 ; k++)
            					{
                					list[join_index].players[k] = list[join_index].players[k+1];
            					}
            					i--;
        					}
    					}
						list[join_index].curr_size--;

						printf("After\n");
						for(int j = 0; j<list[join_index].curr_size;j++)
						{
							printf("%s: %d\n", list[join_index].players[j].name, list[join_index].players[j].socket);
						}

						write(fd, "OK", strlen("OK"));
					}
					else if(!strncmp("CANCEL", buf, 5) && fd == list[creator_index].admin_sock)
					{
						write(fd, "OK", strlen("OK"));
						printf("Cancelling Group %d...\ngi: %d", creator_index, group_index);
						fflush(stdout);
						for(int j = 1;j<list[creator_index].curr_size;j++)
						{
							write(list[creator_index].players[j].socket, "ENDGROUP", strlen("ENDGROUP"));
						}
						remove_group(list[creator_index].admin_sock);
					}
					else
						printf("BAD\n");
				}
			}

		}
	}
}
void *run_quiz(void *in)
{
	int qnum, r, cc;
	printf("STARTING QUIZ\n");
	fflush(stdout);
	int i;
	char msg[BUFSIZE];
	int index = (int)in;

	fd_set rfds, wfds, efds;
	fd_set afds;
	int nfds = 0;
	int admin = list[index].admin_sock;
	int curr_size = list[index].curr_size;
	int max_size = list[index].max_size;

	nfds = list[index].players[curr_size - 1].socket + 1;
	printf("number of players in group: %d\n",curr_size);

	FD_ZERO(&afds);
	FD_ZERO(&rfds);
	/*
	FD_ZERO(&bfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	*/


	FD_SET(list[index].admin_sock, &afds);
	FD_CLR(list[index].admin_sock, &afds_main);
	FD_CLR(list[index].admin_sock, &rfds_main);

	for(i=0;i<curr_size;i++)
	{
		printf("joining player %s with socket %d\n", list[index].players[i].name,list[index].players[i].socket);
		fflush(stdout);

		FD_CLR(list[index].players[i].socket, &afds_main);
		FD_CLR(list[index].players[i].socket, &rfds_main);
		if ( nfds_main == list[index].players[i].socket+1 )
			nfds_main--;
			
		FD_SET(list[index].players[i].socket, &afds);
		if (list[index].players[i].socket + 1 > nfds)
			nfds = list[index].players[i].socket+1;
			
	}

	printf("nfds: %d\n",nfds);

    printf("QUEST_NUM is: %d\n", qnum + 1);
    fflush(stdout);
    for(qnum = 0; qnum < list[index].quiz.number_of_questions; qnum++)
    {
		char msg[BUFSIZ];
		Player winner;
		char quest[BUFSIZ];
		char options[BUFSIZ];
		strcat(options, list[index].quiz.questions[qnum]);
		//"QUES|size|full-question-text"
		
		sprintf(quest, "QUES|%d|%s", strlen(list[index].quiz.questions[qnum]), list[index].quiz.questions[qnum]);

		for(r = 0; r < list[index].curr_size; r++)
            write(list[index].players[r].socket, quest, strlen(quest));

		int answers = 0;
		while (answers < list[index].curr_size)
		{
		printf("QUESTION NUMBER is: %d\n", qnum + 1);
        memcpy((char *)&rfds, (char *)&afds, sizeof(rfds));

        if (select(nfds, &rfds, (fd_set *)0, (fd_set *)0, NULL) < 0)
        {
            fprintf( stderr, "server select: %s\n", strerror(errno) );
            exit(-1);
        }
        for(int i = 0; i < nfds; i++)
        {
            if (FD_ISSET( i, &rfds) )
            {
				int answered = 0;
                if (i + 1 > nfds)
                {
                    nfds = i + 1;
                }

				int player_index = 0;
				for(r = 0; r < list[index].curr_size; r++)
        		{
					if(list[index].players[r].socket == i)
						player_index = r;
       			}

                if ( (cc = read( i, buf, BUFSIZE )) <= 0 )
                {
                    printf( "The client has gone.\n" );
                    (void) close(i);
                    FD_CLR( i, &afds );
                    list[index].curr_size--;
                    if ( nfds == i + 1 ) 
						nfds--;
					continue;
                }
                else
                {
					if(!strncmp(buf, "ANS", 3) && !answered)
					{
						//strtok(buf, "|");
						char *temp = strtok(buf, "|");
						char *ans = strtok(NULL, "|");
						printf("Client at %d answered %s", i, ans);
						fflush(stdout);
						
						
						if(!strncmp(ans, "NOANS", 5))
						{
							answers++;
							answered = 1;
							printf("(no answer)\n");
						}

						else if(process_answer(ans[0], list[index].quiz.correct_answers[qnum]) == 0)
						{
							answers++;
							answered = 1;
							if(list[index].players[player_index].score > 0)
								list[index].players[player_index].score--;	
							printf("(wrong answer)\n");
						}

						else if(process_answer(ans[0], list[index].quiz.correct_answers[qnum]) == 1)
						{	
							answers++;
							answered = 1;
							list[index].players[player_index].score++;
							printf("(correct answer)\n");
							if(answers == 1)
								winner = list[index].players[player_index];
						}
						else
							printf(" (invalid answer)\n");

					}
					else if(answered)
					{
						printf("User already answerd\n");
					}
					else
					{
						printf("Invalid answer\n");
					}
                }
				//write(i, msg, strlen(msg));
            }
        }
		}

		char win[BUFSIZ];
		//WIN|name
		sprintf(win, "WIN|%s%s", winner.name, CRLF );
		for(r = 0; r < list[index].curr_size; r++)
        {
            write(list[index].players[r].socket, win, strlen(win));
        }
    }

	char score[BUFSIZ];
	score[0] = '\0';
	strcpy(score, "RESULT");
	//RESULT|name|score|name|score
	for(r = 0; r < list[index].curr_size; r++)
	{
		char player_score[BUFSIZ];
		sprintf(player_score, "|%s|%d", list[index].players[r].name, list[index].players[r].score);
		strcat(score, player_score);
		printf("adding scores of %s\n", list[index].players[r].name);fflush(stdout);
	}
	strcat(score, CRLF);
	printf("sending [%s]\n",score);
	for(r = 0; r < list[index].curr_size; r++)
    {
		printf("sending scores to %s\n", list[index].players[r].name);fflush(stdout);
		write(list[index].players[r].socket, score, strlen(score));
    }
	for(r = 0; r < list[index].curr_size; r++)
    {		
		FD_CLR(list[index].players[r].socket, &afds);
		
		printf("rejoining %s back to main\n", list[index].players[r].name);fflush(stdout);
		FD_SET(list[index].players[r].socket, &afds_main);
		if ( list[index].players[r].socket+1 > nfds_main )
			nfds_main = list[index].players[r].socket+1;

		write(list[index].players[r].socket, "ENDGROUP", strlen("ENDGROUP"));
    }

	printf("exited\n");fflush(stdout);
	remove_group(admin);
	//pthread_exit(0);
}

int process_answer(char ans, char right_ans)
{
	if(!isalpha(ans))
		return(-1);
	ans = toupper(ans);
	if (ans == right_ans)
		return(1);
	return(0);
}

int search_group(char* groupname)
{
	int i;

	for(i = 0; i <= group_index; i++)
		if(!strcmp(groupname, list[i].name))
			return(i);
	return(-1);
}

Quiz get_quiz(char *quiztext) 
{
	Quiz newQuiz;
	newQuiz.number_of_questions = 0;

    int qs = 0;
    //FILE *fquiz = fopen("q.txt", "r");
    int i = 0;
	int total = 0;

    char *p = strtok (quiztext, "\n");
    char *array[128];

    while (p != NULL)
    {
        array[i++] = p;
        p = strtok (NULL, "\n");
        total++;
    }
    i = 0;

    int pa = 0;
    while(i < total)
    {
        strcpy(newQuiz.questions[qs], array[i]);
        //printf("read: %s\n", questions[qs]);
        i++;
        while(strlen(array[i]) != 1)
        {
            strcat(newQuiz.questions[qs], "\n");
            strcat(newQuiz.questions[qs], array[i]);
            i++;
        }
        strcpy(&newQuiz.correct_answers[qs], array[i]);
        i++;qs++;
    }

	newQuiz.number_of_questions = qs;

	printf("Created quiz with %d questions\n",newQuiz.number_of_questions);

	//for(i=0;i<newQuiz.number_of_questions;i++)
	//	printf("Question %d\n%s\n\n", i, newQuiz.questions[i]);

    return newQuiz;
}

void remove_group(int admin_sock)
{
	for(int i = 0; i<32; i++)
    {
    	if(admin_sock == list[i].admin_sock)
    	{
            for (int k = i; k<31 ; k++)
        	{
                list[k] = list[k+1];
        	}
        	i--;
    	}
    }
	printf("out\n");fflush(stdout);
	group_index--;
}