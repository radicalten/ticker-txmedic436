/*
 Minimal Terminal Chess GUI with UCI support
 macOS Terminal compatible
 Single file, no external dependencies
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <time.h>

#define MAX_MOVES 512
#define MAX_PGN   2048

/* ================= TERMINAL CONTROL ================= */

void clear_screen() { printf("\033[2J\033[H"); }
void move_cursor(int r, int c){ printf("\033[%d;%dH", r, c); }

void enable_raw() {
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}
void disable_raw() {
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

/* ================= BOARD ================= */

char board[8][8];
int white_to_move = 1;
char last_move[6] = "";
char pgn[MAX_PGN] = "";

typedef struct { char from[3], to[3]; } Move;
Move history[MAX_MOVES];
int history_len = 0;

FILE *engine = NULL;

/* ================= PIECES ================= */

char *unicode(char p){
    switch(p){
        case 'P': return "♙"; case 'p': return "♟";
        case 'R': return "♖"; case 'r': return "♜";
        case 'N': return "♘"; case 'n': return "♞";
        case 'B': return "♗"; case 'b': return "♝";
        case 'Q': return "♕"; case 'q': return "♛";
        case 'K': return "♔"; case 'k': return "♚";
        default: return " ";
    }
}

void init_board(){
    char *start[8] = {
        "rnbqkbnr",
        "pppppppp",
        "........",
        "........",
        "........",
        "........",
        "PPPPPPPP",
        "RNBQKBNR"
    };
    for(int r=0;r<8;r++)
        for(int c=0;c<8;c++)
            board[r][c] = start[r][c];
}

/* ================= DRAW ================= */

int sel_r=-1, sel_c=-1;

int in_bounds(int r,int c){ return r>=0&&r<8&&c>=0&&c<8; }

void draw_square(int r,int c){
    int dark = (r+c)%2;
    char p = board[r][c];

    int highlight = 0;
    if(sel_r==r && sel_c==c)
        printf("\033[42m"); // green selection
    else if(strncmp(last_move,"",1)!=0){
        int fr = 8-(last_move[1]-'0');
        int fc = last_move[0]-'a';
        int tr = 8-(last_move[3]-'0');
        int tc = last_move[2]-'a';
        if((r==fr&&c==fc)||(r==tr&&c==tc))
            printf("\033[44m"); // blue last move
        else
            printf(dark?"\033[48;5;94m":"\033[48;5;180m");
    } else
        printf(dark?"\033[48;5;94m":"\033[48;5;180m");

    printf(" %s ", unicode(p));
    printf("\033[0m");
}

void draw_board(){
    clear_screen();
    for(int r=0;r<8;r++){
        printf("%d ",8-r);
        for(int c=0;c<8;c++)
            draw_square(r,c);
        printf("\n");
    }
    printf("   a  b  c  d  e  f  g  h\n\n");
    printf("Turn: %s\n", white_to_move?"White":"Black");
    printf("Moves: %s\n", pgn);
}

/* ================= MOVE LOGIC ================= */

int is_white(char p){ return isupper(p); }
int is_black(char p){ return islower(p); }

void make_move(char *from,char *to){
    int fr=8-(from[1]-'0'), fc=from[0]-'a';
    int tr=8-(to[1]-'0'), tc=to[0]-'a';

    board[tr][tc]=board[fr][fc];
    board[fr][fc]='.';

    strcpy(last_move,from);
    strcat(last_move,to);

    history[history_len].from[0]=from[0];
    history[history_len].from[1]=from[1];
    history[history_len].from[2]=0;
    history[history_len].to[0]=to[0];
    history[history_len].to[1]=to[1];
    history[history_len].to[2]=0;
    history_len++;

    strcat(pgn, from);
    strcat(pgn, to);
    strcat(pgn, " ");

    white_to_move=!white_to_move;
}

void undo(){
    if(history_len==0) return;
    history_len--;
    Move m=history[history_len];

    int fr=8-(m.from[1]-'0'), fc=m.from[0]-'a';
    int tr=8-(m.to[1]-'0'), tc=m.to[0]-'a';

    board[fr][fc]=board[tr][tc];
    board[tr][tc]='.';

    white_to_move=!white_to_move;
    pgn[0]=0;
    for(int i=0;i<history_len;i++){
        strcat(pgn, history[i].from);
        strcat(pgn, history[i].to);
        strcat(pgn, " ");
    }
}

/* ================= UCI ================= */

void send_engine(char *cmd){
    fprintf(engine,"%s\n",cmd);
    fflush(engine);
}

void get_engine_move(){
    char buf[256];
    send_engine("position startpos moves");
    for(int i=0;i<history_len;i++){
        fprintf(engine,"%s%s ",history[i].from,history[i].to);
    }
    fprintf(engine,"\n");

    send_engine("go depth 10");

    while(fgets(buf,256,engine)){
        if(strncmp(buf,"bestmove",8)==0){
            char move[6];
            sscanf(buf,"bestmove %5s",move);
            char from[3]={move[0],move[1],0};
            char to[3]={move[2],move[3],0};
            make_move(from,to);
            break;
        }
    }
}

/* ================= INPUT ================= */

void handle_input(){
    int ch=getchar();
    if(ch==27){ getchar(); ch=getchar();
        if(ch=='A'&&sel_r>0)sel_r--;
        if(ch=='B'&&sel_r<7)sel_r++;
        if(ch=='C'&&sel_c<7)sel_c++;
        if(ch=='D'&&sel_c>0)sel_c--;
    } else if(ch==' '){
        if(sel_r>=0){
            static int fr=-1,fc=-1;
            if(fr==-1){ fr=sel_r; fc=sel_c; }
            else{
                char from[3]={fc+'a',8-fr+'0',0};
                char to[3]={sel_c+'a',8-sel_r+'0',0};
                make_move(from,to);
                fr=-1;
                get_engine_move();
            }
        }
    } else if(ch=='u') undo();
    else if(ch=='q'){ disable_raw(); exit(0); }
}

/* ================= MAIN ================= */

int main(int argc,char**argv){
    if(argc<2){
        printf("Usage: %s /path/to/uci_engine\n",argv[0]);
        return 1;
    }

    engine=popen(argv[1],"r+");
    if(!engine){ printf("Engine error\n"); return 1; }

    send_engine("uci");
    send_engine("isready");

    init_board();
    enable_raw();
    sel_r=7; sel_c=0;

    while(1){
        draw_board();
        handle_input();
    }
}
