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
#define ADMIN		"QS|ADMIN\r\n"
#define JOIN		"QS|JOIN\r\n"
#define FULL		"QS|FULL\r\n"
#define WAIT		"WAIT\r\n"
#define CRLF        "\r\n"

void* run_quiz(void* ssock);
void get_quiz(char *filename);

int clients = 0;

int passivesock( char *service, char *protocol, int qlen, int *rport );
int process_answer(char ans, char right_ans);
void *serve(void *ssock);

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

char buf[BUFSIZE];

int main( int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("missing quiz .txt file\n");
        exit(-1);
    }
	char			*service;
	struct sockaddr_in	fsin;
	socklen_t		alen;
	int			msock;
	int			ssock;
	int			rport = 0;
	
	switch (argc) 
	{
		case	1:
			// No args? let the OS choose a port and tell the user
			rport = 1;
            get_quiz(argv[1]);
			break;
		case	2:
			// User provides a port? then use it
            rport = 1;
            get_quiz(argv[1]);
			break;
        case    3:
            service = argv[1];
            get_quiz(argv[2]);
			break;
		default:
			fprintf( stderr, "usage: server [port]\n" );
			exit(-1);
	}

	msock = passivesock( service, "tcp", QLEN, &rport );
	if (rport)
	{
		//	Tell the user the selected port
		printf( "server: port %d\n", rport );	
		fflush( stdout );
	}
	
	for (;;)
	{
		int	ssock;

        pthread_t pid;
		alen = sizeof(fsin);
		ssock = accept( msock, (struct sockaddr *)&fsin, &alen );
		if (ssock < 0)
		{
			fprintf( stderr, "accept: %s\n", strerror(errno) );
			exit(-1);
		}
        clients++;

        printf( "client %d has arrived.\n", clients );
		fflush( stdout );

        if (clients == 1)
			write(ssock, ADMIN, strlen(ADMIN));
		else
			write(ssock, JOIN, strlen(JOIN));

        pthread_create(&pid, NULL, serve, (void *) ssock);
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
	fd_set afds;
	int nfds = 0;
	int admin = group.admin_sock;
	int curr_size = group.curr_size;
	int max_size = group.max_size;

	nfds = group.players[curr_size - 1].socket + 1;
	
    printf("number of players in group: %d\n",curr_size);

	FD_ZERO(&afds);
	FD_ZERO(&rfds);
	/*
	FD_ZERO(&bfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	*/


	FD_SET(group.admin_sock, &afds);

	for(i=0;i<curr_size;i++)
	{
		printf("joining player %s with socket %d\n", group.players[i].name,group.players[i].socket);
		fflush(stdout);
			
		FD_SET(group.players[i].socket, &afds);
		if (group.players[i].socket + 1 > nfds)
			nfds = group.players[i].socket+1;
			
	}

	printf("nfds: %d\n",nfds);

    printf("QUEST_NUM is: %d\n", qnum + 1);
    fflush(stdout);

    for(qnum = 0; qnum < theQuiz.number_of_questions; qnum++)
    {
        printf("QUESTION NUMBER is: %d\n", qnum + 1);
		char msg[BUFSIZ];
		Player winner;
		char quest[BUFSIZ];
		//"QUES|size|full-question-text"
		sprintf(quest, "QUES|%d|%s", strlen(theQuiz.questions[qnum]), theQuiz.questions[qnum]);
		for(r = 0; r < group.curr_size; r++)
        {
            write(group.players[r].socket, quest, strlen(quest));
        }

		int answers = 0;
		while (answers < group.curr_size)
		{
        printf("waiting... q\n");
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
                    group.curr_size--;
                    if ( nfds == i + 1 ) 
						nfds--;
                }
                else
                {
					if(!strncmp(buf, "ANS", 3) && i != group.admin_sock)
					{
						//strtok(buf, "|");
						char *temp = strtok(buf, "|");
						char *ans = strtok(NULL, "|");
						printf("Client at %d answered %s", i, ans);
						fflush(stdout);
						
						if(process_answer(ans[0], theQuiz.correct_answers[qnum]) == 0)
						{
							answers++;
							printf(" (wrong answer)\n");
						}

						else if(process_answer(ans[0], theQuiz.correct_answers[qnum]) == 1)
						{	
							answers++;
							group.players[player_index].score++;
							printf(" (correct answer)\n");
							if(answers == 1)
								winner = group.players[player_index];
						}
						else
							printf(" (invalid answer)\n");

					}
                }
				//write(i, msg, strlen(msg));
                printf("Client at %d answered %s\n", i, buf);
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
        close(group.players[r].socket);
    }
    close(group.admin_sock);
    clients = 0;
    pthread_exit(0);
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
void *serve(void *ssock)
{
    struct Player p;
	p.score = 0;
	p.in_quiz = 0;
	p.socket = ssock;
	char msg[BUFSIZE];
    int cc;
    for (;;)
	{
        if(p.in_quiz)
            break;
        else
            printf("not in quiz\n");
        printf("waiting...\n");
		if ( (cc = read( ssock, buf, BUFSIZE )) <= 0 )
		{
			printf( "A client has gone.\n" );
			close(ssock);
			break;
		}
        else
		{	
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
				group.admin_sock = ssock;

				printf("Created Group: %s\nCurrent players: %d\n%d maximum players\nadmin socket: %d\n", group.name,group.curr_size,group.max_size, ssock);
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
						send(ssock, FULL, strlen(FULL), 0);
						continue;
					}
					else if(group.curr_size < group.max_size - 1)
					{
						printf("\"%s\" joined the group\n",p.name);
						strcpy(msg , "Joined the group");
		

						pthread_mutex_lock(&mutexJoin);
						write(ssock, WAIT, strlen(WAIT));
						group.players[group.curr_size] = p;
						group.curr_size++;
						if (group.curr_size != group.max_size )
							pthread_cond_wait(&condGroup, &mutexJoin);
						else
							pthread_cond_broadcast(&condGroup);
						pthread_mutex_unlock(&mutexJoin);
                        //close(ssock);
                        p.in_quiz = 1;
						
					}
					else if(group.curr_size == group.max_size - 1)
					{
						write(ssock, WAIT, strlen(WAIT));
						group.players[group.curr_size] = p;
						group.curr_size++;
                        p.in_quiz = 1;

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
            else
            {
                printf("still in main...");
            }
		}
        
	}
    printf("Exited\n");
}

void get_quiz(char *filename)
{
    printf("Quiz: %s\n",filename); fflush(stdout);
    int qs = 0;
    int i= 0;
    FILE* file = fopen(filename, "r");
    char temp[128];
    while(fgets(temp, sizeof(temp), file))
    {
        char *line = "";
        if(strlen(temp) != 1)
            line = strtok(temp,"\n");
        //printf("Received: %s\n",line); fflush(stdout);
        if(strlen(line) == 1 )
        {
            theQuiz.correct_answers[qs] = line[0];
            qs++;
        }
        else
        {
            strcat(theQuiz.questions[qs], "\n");
            strcat(theQuiz.questions[qs], line);
        }
    }
    theQuiz.number_of_questions = qs;
	/*
    for(i = 0; i < theQuiz.number_of_questions; i++)
    {
        printf("[Question] %s\n Answer: %c\n", theQuiz.questions[i], theQuiz.correct_answers[i]);
    }
	*/
    printf("Created quiz with %d questions\n",theQuiz.number_of_questions);
    fflush(stdout);
}