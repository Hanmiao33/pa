#include "nemu.h"

/* We use the POSIX regex functions to process regular expressions.
 * Type 'man regex' for more information about POSIX regex functions.
 */
#include <sys/types.h>
#include <regex.h>

enum {
  TK_NOTYPE = 256, TK_EQ,

  /* TODO: Add more token types */
  TK_NEQ,
  TK_AND,
  TK_OR,
  TK_NOT,
  TK_NUM,
  TK_REG,
  TK_DEREF,

};

static struct rule {
  char *regex;
  int token_type;
} rules[] = {

  /* TODO: Add more rules.
   * Pay attention to the precedence level of different rules.
   */

  {" +", TK_NOTYPE},    // spaces
  {"0x[0-9a-fA-F]+|\\d+", TK_NUM}, // decimal and hex numbers
  {"\\$[a-zA-Z]+", TK_REG},        // registers ($eax, $ebx...)
  {"==", TK_EQ},         // equal
  {"!=", TK_NEQ},        // not equal
  {"&&", TK_AND},        // logical and
  {"\\|\\|", TK_OR},     // logical or
  {"!", TK_NOT},         // logical not
  {"\\+", '+'},          // plus
  {"-", '-'},            // minus
  {"\\*", '*'},          // multiply
  {"/", '/'},            // divide
  {"\\(", '('},          // left parenthesis
  {"\\)", ')'},          // right parenthesis
};

#define NR_REGEX (sizeof(rules) / sizeof(rules[0]) )

static regex_t re[NR_REGEX];

/* Rules are used for many times.
 * Therefore we compile them only once before any usage.
 */
void init_regex() {
  int i;
  char error_msg[128];
  int ret;

  for (i = 0; i < NR_REGEX; i ++) {
    ret = regcomp(&re[i], rules[i].regex, REG_EXTENDED);
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
    /* Try all rules one by one. */
    for (i = 0; i < NR_REGEX; i ++) {
      if (regexec(&re[i], e + position, 1, &pmatch, 0) == 0 && pmatch.rm_so == 0) {
        char *substr_start = e + position;
        int substr_len = pmatch.rm_eo;

        Log("match rules[%d] = \"%s\" at position %d with len %d: %.*s",
            i, rules[i].regex, position, substr_len, substr_len, substr_start);
        position += substr_len;

        /* TODO: Now a new token is recognized with rules[i]. Add codes
         * to record the token in the array `tokens'. For certain types
         * of tokens, some extra actions should be performed.
         */

        switch (rules[i].token_type) {
          case TK_NOTYPE:
            break;
          default:
            tokens[nr_token].type = rules[i].token_type;
            strncpy(tokens[nr_token].str, substr_start, substr_len);
            tokens[nr_token].str[substr_len < 32 ? substr_len : 31] = '\0';
            nr_token++;
        }

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
  for (int i = l; i <= r; i++) {
    if (tokens[i].type == '(') cnt++;
    else if (tokens[i].type == ')') cnt--;
    if (cnt == 0 && i != r) return false;
  }
  return cnt == 0;
}

static int get_priority(int op) {
  switch (op) {
    case TK_OR: return 0;
    case TK_AND: return 1;
    case TK_EQ: case TK_NEQ: return 2;
    case '+': case '-': return 3;
    case '*': case '/': return 4;
    case TK_NOT: case TK_DEREF: return 5;
    default: return -1;
  }
}

static int find_dominant_op(int l, int r) {
  int op = -1, pri = 100, cnt = 0;
  for (int i = l; i <= r; i++) {
    int t = tokens[i].type;
    if (t == '(') cnt++;
    else if (t == ')') cnt--;
    if (cnt != 0) continue;

    int p = get_priority(t);
    if (p < 0) continue;
    if (p <= pri) {
      op = i;
      pri = p;
    }
  }
  return op;
}

static uint32_t eval(int l, int r, bool *success) {
  if (l > r) {
    *success = false;
    return 0;
  }

  if (l == r) {
    if (tokens[l].type == TK_NUM) {
      if (strstr(tokens[l].str, "0x") || strstr(tokens[l].str, "0X")) {
        return strtoul(tokens[l].str, NULL, 16);
      } else {
        return atoi(tokens[l].str);
      }
    }
    if (tokens[l].type == TK_REG) {
      for (int i = 0; i < 8; i++) {
        if (strcmp(regsl[i], tokens[l].str + 1) == 0) {
          return reg_l(i);
        }
      }
      *success = false;
      return 0;
    }
    *success = false;
    return 0;
  }

  if (check_parentheses(l, r)) {
    return eval(l + 1, r - 1, success);
  }

  int op = find_dominant_op(l, r);
  if (op == -1) {
    *success = false;
    return 0;
  }
  int t = tokens[op].type;

  if (t == TK_NOT || t == TK_DEREF) {
    uint32_t val = eval(op + 1, r, success);
    if (!*success) return 0;
    if (t == TK_NOT) return !val;
    else return vaddr_read(val, 4);
  }

  uint32_t val1 = eval(l, op - 1, success);
  if (!*success) return 0;
  uint32_t val2 = eval(op + 1, r, success);
  if (!*success) return 0;

  switch (t) {
    case '+': return val1 + val2;
    case '-': return val1 - val2;
    case '*': return val1 * val2;
    case '/': return val2 == 0 ? 0 : val1 / val2;
    case TK_EQ: return val1 == val2;
    case TK_NEQ: return val1 != val2;
    case TK_AND: return val1 && val2;
    case TK_OR: return val1 || val2;
    default:
      *success = false;
      return 0;
  }
}

uint32_t expr(char *e, bool *success) {
  if (!make_token(e)) {
    *success = false;
    return 0;
  }

  /* TODO: Insert codes to evaluate the expression. */
  
  for (int i = 0; i < nr_token; i++) {
    if (tokens[i].type == '*') {
      if (i == 0) {
        tokens[i].type = TK_DEREF;
      } else {
        int prev_type = tokens[i-1].type;
        if (prev_type == '+' || prev_type == '-' || prev_type == '*' || prev_type == '/' ||
            prev_type == '(' || prev_type == TK_EQ || prev_type == TK_NEQ ||
            prev_type == TK_AND || prev_type == TK_OR || prev_type == TK_NOT) {
          tokens[i].type = TK_DEREF;
        }
      }
    }
  }

  return eval(0, nr_token - 1, success);
}
