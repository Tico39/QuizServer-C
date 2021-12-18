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


#define	QLEN			5
#define	BUFSIZE			4096
#define ADMIN		"QS|ADMIN\r\n"
#define JOIN		"QS|JOIN\r\n"
#define FULL		"QS|FULL\r\n"
#define WAIT		"WAIT\r\n"
#define CRLF        "\r\n"

void* run_quiz(void* ssock);

int clients = 0;

int passivesock( char *service, char *protocol, int qlen, int *rport );
int process_answer(char ans, char right_ans);

typedef struct Player 
{
	char     name[50];
    int      score;
    int      in_quiz;
    int      socket;
} Player;

typedef struct Quiz 
{
	int number_of_questions;
    char questions[128][128];
    char correct_answers[128];
    char possible_answers[3][128][128];
    char winner[50];
} Quiz;

typedef struct Group 
{
    char            name[50];
    int             max_size;
    int				quest_num;
    Player          players[50];
    int             admin_sock;
    int             curr_size;
} Group;

Group group;
Quiz theQuiz;

pthread_mutex_t mutexJoin;
pthread_mutex_t mutexInQuiz;
pthread_cond_t condGroup;
pthread_cond_t condInQuiz;

fd_set afds_main;
char buf[BUFSIZE];

int
main( int argc, char *argv[] )
{
	char			buf[BUFSIZE];
	char			*service;
	struct sockaddr_in	fsin;
	int			msock;
	int			ssock;
	fd_set		rfds;
	int			alen;
	int			fd;
	int			nfds;
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
	nfds = msock+1;

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
		memcpy((char *)&rfds, (char *)&afds_main, sizeof(rfds));

		// Only waiting for sockets who are ready to read
		//  - this includes new clients arriving
		//  - this also includes the client closed the socket event
		// We pass null for the write event and exception event fd_sets
		// we pass null for the timer, because we don't want to wake early
		if (select(nfds, &rfds, (fd_set *)0, (fd_set *)0,
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
		if (FD_ISSET( msock, &rfds)) 
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

			if ( ssock+1 > nfds )
				nfds = ssock+1;
			
			if (clients == 0)
				write(ssock, ADMIN, strlen(ADMIN));
			else
				write(ssock, JOIN, strlen(JOIN));
		}

		// Now check all the regular sockets
		for ( fd = 0; fd < nfds; fd++ )
		{		
			char msg[BUFSIZE];

			if (fd != msock && FD_ISSET(fd, &rfds))
			{
				struct Player p;
				p.score = 0;
				p.in_quiz = 0;
				p.socket = fd;
					
				if ( (cc = read( fd, buf, BUFSIZE )) <= 0 )
				{
					printf( "The client has gone.\n" );
					(void) close(fd);
					
					FD_CLR( fd, &afds_main );

					if ( nfds == fd+1 )
						nfds--;
				}
				else
				{
					clients++;
					
					if (!strncmp("GROUP", buf, 5))
					{
						int group_size;
  						char *start = &buf[5];
  						char *end = &buf[cc];
 						char *substr = (char *)calloc(1, end - start + 1);
  						memcpy(substr, start, end - start);
				
						char *Lname = strtok(substr, "|");
						char *Lnum = strtok(NULL, "|");

						if (atoi(Lnum) > 0)
						{
							group_size = atoi(Lnum);
							strcpy(msg ,"Joined the group");
						}
						else
							strcpy(msg , "BAD");
				
						group.name[0] = '\0';
						strcpy(group.name, "");
						strcat(group.name, Lname);

						group.max_size = atoi(Lnum);
						group.curr_size = 0;
						group.admin_sock = fd;

						printf("Created Group: %s\nCurrent players: %d\n%d maximum players\nadmin socket: %d\n", group.name,group.curr_size,group.max_size, fd);
					}
					else if(!strncmp("JOIN", buf, 4))
					{
						strtok(buf, "|");
						char *name  = strtok(NULL, "|");
						printf("subtring: [%s]\n", buf);
						if(!(!strcmp(group.name, "")))
						{
							printf("Group exists\n");
							p.name[0] = '\0';
							strcpy(p.name, "");
							strcat(p.name, strtok(name, CRLF));

							printf("Client \"%s\" entered with socket: %d\n",p.name, p.socket);
							if(group.curr_size >= group.max_size)
							{
								printf("FULL");
								send(fd, FULL, strlen(FULL), 0);
								continue;
							}
							else if(group.curr_size < group.max_size - 1)
							{
								printf("\"%s\" joined the group\n",p.name);
								strcpy(msg , "Joined the group");
							
								FD_CLR( fd, &afds_main );
								if ( nfds == fd+1 )
									nfds--;

								pthread_mutex_lock(&mutexJoin);
								write(fd, WAIT, strlen(WAIT));
								group.players[group.curr_size] = p;
								group.curr_size++;
								if (group.curr_size != group.max_size )
									pthread_cond_wait(&condGroup, &mutexJoin);
								else
									pthread_cond_broadcast(&condGroup);
								pthread_mutex_unlock(&mutexJoin);

								printf("exited\n");
								fflush(stdout);
							}
							else if(group.curr_size == group.max_size - 1)
							{
								write(fd, WAIT, strlen(WAIT));
								group.players[group.curr_size] = p;
								group.curr_size++;
								FD_CLR(fd, &afds_main);
								if ( nfds == fd+1 )
									nfds--;

								pthread_t quiz;

								if(pthread_create(&quiz, NULL, run_quiz, (void*)group.name) < 0)
								{
									printf(stderr, "ERROR: %s\n", strerror(errno));
									exit(-1);
								}
							}
						}
						else
						{
							printf("Group doesn't exist\n");
						}
					}
				}
			}

		}
	}
}
void *run_quiz(void *ssock)
{
	int qnum, r, cc;
	printf("STARTING QUIZ\n");
	fflush(stdout);
	int i;
	char msg[BUFSIZE];

	fd_set rfds, wfds, efds;
	fd_set afds, bfds;
	int nfds = 0;
	int admin = group.admin_sock;
	int curr_size = group.curr_size;
	int max_size = group.max_size;

	nfds = group.admin_sock + curr_size + 1;
	printf("nfds: %d\n",nfds);
	printf("number of players in group: %d\n",curr_size);

	FD_ZERO(&afds);
	FD_ZERO(&bfds);
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);

	FD_SET(group.admin_sock, &afds);
	FD_CLR(group.admin_sock, &afds_main);
	
	for(i=0;i<curr_size*2 + 1;i++)
	{
		/*if(group.players[i].socket != 0)
		{*/
			printf("joining player with socket %d\n",group.players[i].socket);
			fflush(stdout);
			group.players[i].in_quiz = 1;
			FD_SET(group.players[i].socket, &afds);
		//}
	}

    printf("QUEST_NUM is: %d\n", group.quest_num + 1);
    fflush(stdout);

	char *question[] = {"this is question 1","this is question 2","this is question 3"};
	char *answer[] = {'A','B','C'};

    for(qnum = 0; qnum < 3; qnum++)
    {
		char msg[BUFSIZ];
		Player winner;
		char quest[BUFSIZ];
		//"QUES|size|full-question-text"
		sprintf(quest, "QUES|%d|%s", strlen(question[qnum]), question[qnum]);
		for(r = 0; r < group.curr_size; r++)
        {
            write(group.players[r].socket, quest, strlen(quest));
        }

		int answers = 0;
		while (answers < group.curr_size)
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
				for(r = 0; r < group.curr_size; r++)
        		{
					if(group.players[r].socket == i)
						player_index = r;
       			}

                if ( (cc = read( i, buf, BUFSIZE )) <= 0 )
                {
                    printf( "The client has gone.\n" );
                    (void) close(i);
                    FD_CLR( i, &afds );
                    if (i == group.admin_sock)
                    {
                        FD_ZERO(&afds);
                        FD_ZERO(&bfds);
                        FD_ZERO(&rfds);
                        FD_ZERO(&wfds);
                        FD_ZERO(&efds);
                        pthread_exit(0);
                    }

                    group.curr_size--;
                    if ( nfds == i + 1 ) 
						nfds--;
                }
                else
                {
                    if(strncmp(buf, "CANCEL", 6) == 0 && i == group.admin_sock) 
					{

                        char* rest2 = buf;
                        strtok_r(rest2, "|", &rest2); 
                        char *gr_name = (char *)malloc(sizeof(char)*256);
                        gr_name = strtok_r(rest2, "|", &rest2); 
						char end[280];
						strcat(end, "|");
						strcat(end, gr_name);
						strcat(end, "\r\n");
                        for (int k = 0; k < group.curr_size; k++)
                        {
                            send(group.players[k].socket, end, strlen(end), 0);
                            close(group.players[k].socket);
                        }
                    }
					else if(!strncmp(buf, "ANS", 3) && i != group.admin_sock && !answered)
					{
						//strtok(buf, "|");
						char *temp = strtok(buf, "|");
						char *ans = strtok(NULL, "|");
						printf("Client at %d answered %s", i, ans);
						fflush(stdout);
						
						if(process_answer(ans[0], answer[qnum]) == 0)
						{
							answers++;
							answered = 1;
							printf(" (wrong answer)\n");
						}

						else if(process_answer(ans[0], answer[qnum]) == 1)
						{	
							answers++;
							answered = 1;
							group.players[player_index].score++;
							printf(" (correct answer)\n");
							if(answers == 1)
								winner = group.players[player_index];
						}
						else
							printf(" (invalid answer)\n");

					}
					else if(answered)
					{
						strcpy(msg , "Already answered");
					}
					else
					{
						strcpy(msg , "Invalid answer");
					}
                }
				//write(i, msg, strlen(msg));
            }
        }
		}

		char win[BUFSIZ];
		//WIN|name
		sprintf(win, "WIN|%s%s", winner.name, CRLF );
		for(r = 0; r < group.curr_size; r++)
        {
            write(group.players[r].socket, win, strlen(win));
        }
    }

	char score[BUFSIZ];
	score[0] = '\0';
	strcpy(score, "RESULTS");
	//RESULT|name|score|name|score
	for(r = 0; r < group.curr_size; r++)
	{
		char player_score[BUFSIZ];
		sprintf(player_score, "|%s|%d", group.players[r].name, group.players[r].score);
		strcat(score, player_score);
	}
	//strcat(score, '\0');

	for(r = 0; r < group.curr_size; r++)
    {
		write(group.players[r].socket, score, strlen(score));
    }
}

int process_answer(char ans, char right_ans)
{
	if( ans != 'A' && ans != 'B' && ans != 'C')
		return(-1);
	if (ans == right_ans)
		return(1);
	return(0);
}
