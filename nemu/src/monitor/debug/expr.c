#include "nemu.h"

#include <sys/types.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

enum {
  TK_NOTYPE = 256, TK_EQ,
  TK_NEQ, TK_AND, TK_OR, TK_NOT,
  TK_NUM, TK_REG, TK_DEREF
};

static struct rule {
  char *regex;
  int token_type;
} rules[] = {
  {" +", TK_NOTYPE},
  {"0x[0-9a-fA-F]+", TK_NUM},
  {"[0-9]+", TK_NUM},
  {"\\$eax|\\$ebx|\\$ecx|\\$edx|\\$esp|\\$ebp|\\$esi|\\$edi", TK_REG},
  {"==", TK_EQ},
  {"!=", TK_NEQ},
  {"&&", TK_AND},
  {"\\|\\|", TK_OR},
  {"!", TK_NOT},
  {"\\+", '+'},
  {"-", '-'},
  {"\\*", '*'},
  {"/", '/'},
  {"\\(", '('},
  {"\\)", ')'}
};

#define NR_REGEX (sizeof(rules) / sizeof(rules[0]) )

static regex_t re[NR_REGEX];

void init_regex() {
  int i;
  char error_msg[128];
  int ret;

  for (i = 0; i < NR_REGEX; i ++) {
    ret = regcomp(&re[i], rules[i].regex, REG_EXTENDED | REG_ICASE);
    if (ret != 0) {
      regerror(ret, &re[i], error_msg, 128);
      panic("regex compilation failed: %s\n%s", error_msg, rules[i].regex);
    }
  }
}

typedef struct token {
  int type;
  char str[32];
} Token;

Token tokens[32];
int nr_token;

static bool make_token(char *e) {
  int position = 0;
  int i;
  regmatch_t pmatch;

  nr_token = 0;

  while (e[position] != '\0') {
    for (i = 0; i < NR_REGEX; i ++) {
      if (regexec(&re[i], e + position, 1, &pmatch, 0) == 0 && pmatch.rm_so == 0) {
        int len = pmatch.rm_eo;
        int type = rules[i].token_type;

        if (type != TK_NOTYPE) {
          tokens[nr_token].type = type;
          strncpy(tokens[nr_token].str, e+position, len);
          tokens[nr_token].str[len>31?31:len] = 0;
          nr_token++;
        }
        position += len;
        break;
      }
    }

    if (i == NR_REGEX) {
      printf("no match at position %d\n%s\n%*.s^\n", position, e, position, "");
      return false;
    }
  }
  return true;
}

static bool check_parentheses(int l, int r) {
  if (tokens[l].type != '(' || tokens[r].type != ')') return false;
  int cnt = 0;
  for (int i=l; i<=r; i++) {
    if (tokens[i].type == '(') cnt++;
    if (tokens[i].type == ')') cnt--;
    if (cnt == 0 && i != r) return false;
  }
  return cnt == 0;
}

static int priority(int op) {
  switch(op) {
    case TK_OR: return 0;
    case TK_AND: return 1;
    case TK_EQ: case TK_NEQ: return 2;
    case '+': case '-': return 3;
    case '*': case '/': return 4;
    case TK_NOT: case TK_DEREF: return 5;
    default: return -1;
  }
}

static int find_op(int l, int r) {
  int op = -1, pri = 999, cnt = 0;
  for (int i=l; i<=r; i++) {
    int t = tokens[i].type;
    if (t == '(') cnt++;
    if (t == ')') cnt--;
    if (cnt != 0) continue;

    int p = priority(t);
    if (p < 0) continue;
    if (p <= pri) {
      op = i;
      pri = p;
    }
  }
  return op;
}

static uint32_t eval(int l, int r, bool *ok) {
  if (l > r) { *ok = 0; return 0; }
  if (l == r) {
    if (tokens[l].type == TK_NUM) {
      if (strstr(tokens[l].str, "0x"))
        return strtoul(tokens[l].str, NULL, 16);
      else
        return atoi(tokens[l].str);
    }
    if (tokens[l].type == TK_REG) {
      if (!strcmp(tokens[l].str, "$eax")) return reg_l(0);
      if (!strcmp(tokens[l].str, "$ecx")) return reg_l(1);
      if (!strcmp(tokens[l].str, "$edx")) return reg_l(2);
      if (!strcmp(tokens[l].str, "$ebx")) return reg_l(3);
      if (!strcmp(tokens[l].str, "$esp")) return reg_l(4);
      if (!strcmp(tokens[l].str, "$ebp")) return reg_l(5);
      if (!strcmp(tokens[l].str, "$esi")) return reg_l(6);
      if (!strcmp(tokens[l].str, "$edi")) return reg_l(7);
    }
    *ok = 0; return 0;
  }

  if (check_parentheses(l, r))
    return eval(l+1, r-1, ok);

  int op = find_op(l, r);
  if (op < 0) { *ok = 0; return 0; }
  int t = tokens[op].type;

  if (t == TK_NOT || t == TK_DEREF) {
    uint32_t v = eval(op+1, r, ok);
    if (!*ok) return 0;
    if (t == TK_NOT) return !v;
    else return vaddr_read(v, 4);
  }

  uint32_t v1 = eval(l, op-1, ok);
  uint32_t v2 = eval(op+1, r, ok);
  if (!*ok) return 0;

  switch(t) {
    case '+': return v1 + v2;
    case '-': return v1 - v2;
    case '*': return v1 * v2;
    case '/': return v2 ? v1 / v2 : 0;
    case TK_EQ: return v1 == v2;
    case TK_NEQ: return v1 != v2;
    case TK_AND: return v1 && v2;
    case TK_OR: return v1 || v2;
    default: *ok=0; return 0;
  }
}

uint32_t expr(char *e, bool *success) {
  *success = true;
  if (!make_token(e)) { *success = false; return 0; }

  for (int i=0; i<nr_token; i++) {
    if (tokens[i].type == '*') {
      if (i == 0) tokens[i].type = TK_DEREF;
      else {
        int p = tokens[i-1].type;
        if (p == '+' || p == '-' || p == '*' || p == '/' || p == '(' ||
            p == TK_EQ || p == TK_NEQ || p == TK_AND || p == TK_OR || p == TK_NOT)
          tokens[i].type = TK_DEREF;
      }
    }
  }

  return eval(0, nr_token-1, success);
}
