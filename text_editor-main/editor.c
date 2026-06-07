#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <ctype.h>

#define UNDO_CAP 512

typedef struct {
    char **lines;
    size_t size;
    size_t cap;
} Buf;

typedef struct {
    int idx;
    char *before;
    char *after;
} Act;

typedef struct {
    Act a[UNDO_CAP];
    int top;
} Stack;

static void binit(Buf *b) {
    b->cap = 64;
    b->size = 0;
    b->lines = malloc(sizeof(char*) * b->cap);
}

static void bfree(Buf *b) {
    for (size_t i = 0; i < b->size; i++) free(b->lines[i]);
    free(b->lines);
}

static void bgrow(Buf *b) {
    if (b->size + 1 <= b->cap) return;
    b->cap *= 2;
    b->lines = realloc(b->lines, sizeof(char*) * b->cap);
}

static void bappend(Buf *b, const char *s) {
    bgrow(b);
    b->lines[b->size++] = strdup(s ? s : "");
}

static void binsert(Buf *b, size_t idx, const char *s) {
    if (idx > b->size) idx = b->size;
    bgrow(b);
    for (size_t i = b->size; i > idx; i--) b->lines[i] = b->lines[i-1];
    b->lines[idx] = strdup(s ? s : "");
    b->size++;
}

static void breplace(Buf *b, size_t idx, const char *s) {
    if (idx >= b->size) return;
    free(b->lines[idx]);
    b->lines[idx] = strdup(s ? s : "");
}

static void bload(Buf *b, const char *name) {
    FILE *f = fopen(name,"r");
    if (!f) return;
    for (size_t i = 0; i < b->size; i++) free(b->lines[i]);
    b->size = 0;

    char *line = NULL;
    size_t len = 0;
    ssize_t n;
    while ((n = getline(&line,&len,f)) != -1) {
        while (n > 0 && (line[n-1]=='\n'||line[n-1]=='\r')) line[--n] = 0;
        bappend(b, line);
    }
    free(line);
    fclose(f);

    if (b->size == 0) bappend(b, "");
}

static void bsave(Buf *b, const char *name) {
    FILE *f = fopen(name,"w");
    if (!f) return;
    for (size_t i = 0; i < b->size; i++)
        fprintf(f,"%s\n", b->lines[i]);
    fclose(f);
}

static void st_init(Stack *s) { s->top = 0; }

static void st_clear(Stack *s) {
    while (s->top > 0) {
        s->top--;
        free(s->a[s->top].before);
        free(s->a[s->top].after);
    }
}

static void st_push(Stack *s, int idx, const char *bef, const char *aft) {
    if (s->top >= UNDO_CAP) {
        free(s->a[0].before);
        free(s->a[0].after);
        for (int i = 1; i < s->top; i++) s->a[i-1] = s->a[i];
        s->top--;
    }
    s->a[s->top].idx = idx;
    s->a[s->top].before = strdup(bef ? bef : "");
    s->a[s->top].after = strdup(aft ? aft : "");
    s->top++;
}

static int st_pop(Stack *s, Act *o) {
    if (s->top == 0) return 0;
    s->top--;
    o->idx = s->a[s->top].idx;
    o->before = strdup(s->a[s->top].before);
    o->after = strdup(s->a[s->top].after);
    free(s->a[s->top].before);
    free(s->a[s->top].after);
    return 1;
}

static void draw(Buf *b, size_t topline, size_t curline, size_t curcol, const char *name) {
    int r,c;
    getmaxyx(stdscr,r,c);
    int rows = r - 1;

    werase(stdscr);

    for (int i = 0; i < rows; i++) {
        size_t idx = topline + i;
        if (idx >= b->size) break;
        mvprintw(i,0,"%4zu: %s", idx+1, b->lines[idx]);
    }

    mvprintw(r-1,0,"[%s] l:%zu c:%zu", name?name:"no name", curline+1, curcol+1);

    move((int)(curline - topline), (int)(curcol + 6));
    refresh();
}

static char *prompt(const char *p) {
    echo();
    nocbreak();
    nodelay(stdscr, FALSE);

    int r,c;
    getmaxyx(stdscr,r,c);

    mvprintw(r-1,0,"%s",p);
    clrtoeol();

    char buf[256] = {0};
    mvgetnstr(r-1, strlen(p), buf, 255);

    noecho();
    cbreak();

    return strdup(buf);
}

void editor(Buf *b, char **fname, Stack *undo, Stack *redo) {
    initscr();
    raw();
    keypad(stdscr, TRUE);
    noecho();

    size_t cl = 0, cc = 0, top = 0;
    char *search_text = NULL;
    char *clipboard = NULL;

    draw(b, top, cl, cc, *fname);

    int ch;
    while ((ch = getch()) != 17) {

        if (ch == 6) {
            if (!search_text) {
                char *p = prompt("search: ");
                if (strlen(p) == 0) {
                    free(p);
                    draw(b, top, cl, cc, *fname);
                    continue;
                }
                search_text = p;
            }

            size_t start = cl;
            size_t i = cl;
            int found = 0;

            while (1) {
                char *pos = strstr(b->lines[i], search_text);
                if (pos) {
                    cl = i;
                    cc = pos - b->lines[i];
                    found = 1;
                    break;
                }
                i++;
                if (i >= b->size) i = 0;
                if (i == start) break;
            }

            if (!found) {
                free(search_text);
                search_text = NULL;
            }

            if (cl < top) top = cl;

            int rr, cc2;
            getmaxyx(stdscr, rr, cc2);
            int rows = rr - 1;
            if (cl >= top + (size_t)rows) top = cl - rows + 1;

            draw(b, top, cl, cc, *fname);
            continue;
        }

        else if (ch == '\n' || ch == KEY_ENTER) {
            char *old = b->lines[cl];
            char *left = strndup(old, cc);
            char *right = strdup(old + cc);
            char *saved = strdup(old);

            breplace(b, cl, left);
            binsert(b, cl + 1, right);

            cl++;
            cc = 0;

            st_push(undo, (int)(cl - 1), saved, b->lines[cl - 1]);
            st_clear(redo);

            free(saved);
            free(left);
            free(right);
        }

        else if (ch == 26) {
            Act a;
            if (st_pop(undo, &a)) {
                if (a.idx >= 0 && (size_t)a.idx < b->size) {
                    breplace(b, a.idx, a.before);
                    st_push(redo, a.idx, a.before, a.after);
                }
                free(a.before);
                free(a.after);
            }
        }

        else if (ch == 25) {
            Act a;
            if (st_pop(redo, &a)) {
                if (a.idx >= 0 && (size_t)a.idx < b->size) {
                    breplace(b, a.idx, a.after);
                    st_push(undo, a.idx, a.before, a.after);
                }
                free(a.before);
                free(a.after);
            }
        }

        else if (ch == 3) {
            free(clipboard);
            clipboard = strdup(b->lines[cl]);
        }

        else if (ch == 24) {
            char *before = strdup(b->lines[cl]);

            free(clipboard);
            clipboard = strdup(before);

            free(b->lines[cl]);
            for (size_t i = cl; i + 1 < b->size; i++)
                b->lines[i] = b->lines[i + 1];
            b->size--;

            if (b->size == 0) {
                bappend(b, "");
                cl = 0;
            } else if (cl >= b->size)
                cl = b->size - 1;

            st_push(undo, cl, before, "");
            st_clear(redo);

            free(before);
        }

        else if (ch == 22) {
            if (clipboard) {
                binsert(b, cl + 1, clipboard);
                st_push(undo, cl + 1, "", clipboard);
                st_clear(redo);
                cl++;
                cc = 0;
            }
        }

        else if (ch == KEY_UP) {
            if (cl > 0) cl--;
            if (cc > strlen(b->lines[cl])) cc = strlen(b->lines[cl]);
            if (cl < top) top = cl;
        }
        else if (ch == KEY_DOWN) {
            if (cl + 1 < b->size) cl++;
            int rr, cc2;
            getmaxyx(stdscr, rr, cc2);
            int rows = rr - 1;
            if (cl >= top + (size_t)rows) top = cl - rows + 1;
            if (cc > strlen(b->lines[cl])) cc = strlen(b->lines![cl]);
        }
        else if (ch == KEY_LEFT) {
            if (cc > 0) cc--;
            else if (cl > 0) {
                cl--;
                cc = strlen(b->lines[cl]);
            }
        }
        else if (ch == KEY_RIGHT) {
            if (cc < strlen(b->lines[cl])) cc++;
            else if (cl + 1 < b->size) {
                cl++;
                cc = 0;
            }
        }

        else if (ch == 19) {
            if (!*fname) {
                char *f = prompt("save as: ");
                if (strlen(f) > 0) *fname = f;
                else free(f);
            }
            if (*fname) bsave(b, *fname);
        }

        else if (isprint(ch)) {
            char *before = strdup(b->lines[cl]);
            size_t len = strlen(before);

            char *tmp = malloc(len + 2);
            memcpy(tmp, before, cc);
            tmp[cc] = (char)ch;
            strcpy(tmp + cc + 1, before + cc);

            breplace(b, cl, tmp);
            cc++;

            st_push(undo, cl, before, b->lines[cl]);
            st_clear(redo);

            free(tmp);
            free(before);
        }

        else if (ch == 127 || ch == KEY_BACKSPACE) {
            if (cc > 0) {
                char *before = strdup(b->lines[cl]);
                size_t len = strlen(before);

                char *tmp = malloc(len);
                memcpy(tmp, before, cc - 1);
                strcpy(tmp + cc - 1, before + cc);

                breplace(b, cl, tmp);
                cc--;

                st_push(undo, cl, before, b->lines[cl]);
                st_clear(redo);

                free(tmp);
                free(before);
            }
        }

        draw(b, top, cl, cc, *fname);
    }

    if (!*fname) {
        char *f = prompt("save as: ");
        if (strlen(f) > 0) *fname = f;
        else free(f);
    }

    if (*fname) bsave(b, *fname);
    if (search_text) free(search_text);
    if (clipboard) free(clipboard);

    endwin();
}

static char *getline_stdin(void) {
    char *s = malloc(256);
    if (!fgets(s, 256, stdin)) s[0] = 0;
    s[strcspn(s, "\n")] = 0;
    return s;
}

int main(void) {
    Buf b;
    binit(&b);
    bappend(&b, "");

    Stack undo, redo;
    st_init(&undo);
    st_init(&redo);

    char *fname = NULL;
    int run = 1;

    while (run) {
        printf("\n1 new\n2 open\n3 view\n4 edit\n5 save\n6 exit\n> ");

        char *t = getline_stdin();
        int c = atoi(t);
        free(t);

        if (c == 1) {
            bfree(&b);
            binit(&b);
            bappend(&b, "");

            st_clear(&undo);
            st_clear(&redo);

            free(fname);
            fname = NULL;
        }
        else if (c == 2) {
            printf("file: ");
            char *f = getline_stdin();

            bload(&b, f);

            free(fname);
            fname = strdup(f);
            free(f);

            st_clear(&undo);
            st_clear(&redo);
        }
        else if (c == 3) {
            for (size_t i = 0; i < b.size; i++)
                printf("%zu: %s\n", i + 1, b.lines[i]);
        }
        else if (c == 4) {
            editor(&b, &fname, &undo, &redo);
        }
        else if (c == 5) {
            if (!fname) {
                printf("save as: ");
                fname = getline_stdin();
            }
            bsave(&b, fname);
        }
        else if (c == 6) {
            run = 0;
        }
    }

    free(fname);
    bfree(&b);
    st_clear(&undo);
    st_clear(&redo);

    return 0;
}
