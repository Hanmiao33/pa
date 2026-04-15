#include "monitor/watchpoint.h"
#include "monitor/expr.h"
#include <string.h>
#include <stdlib.h>
#define NR_WP 32

static WP wp_pool[NR_WP];
static WP *head, *free_;

void init_wp_pool() {
  int i;
  for (i = 0; i < NR_WP; i ++) {
    wp_pool[i].NO = i;
    wp_pool[i].next = &wp_pool[i + 1];
  }
  wp_pool[NR_WP - 1].next = NULL;

  head = NULL;
  free_ = wp_pool;
}

/* TODO: Implement the functionality of watchpoint */

// 检查所有监视点，返回是否有变化
bool check_watchpoints() {
  bool changed = false;
  WP *p = head;
  while (p != NULL) {
    bool success;
    uint32_t new_val = expr(p->expr, &success);
    if (success && new_val != p->old_val) {
      printf("Watchpoint %d: %s\n", p->NO, p->expr);
      printf("Old value = 0x%08x\n", p->old_val);
      printf("New value = 0x%08x\n", new_val);
      p->old_val = new_val;
      changed = true;
    }
    p = p->next;
  }
  return changed;
}

// 打印所有监视点
void list_watchpoints() {
  if (head == NULL) {
    printf("No watchpoints.\n");
    return;
  }
  printf("Num     What\n");
  WP *p = head;
  while (p != NULL) {
    printf("%-8d%s\n", p->NO, p->expr);
    p = p->next;
  }
}

// 删除指的监视点
bool delete_watchpoint(int no) {
  WP *p = head;
  while (p != NULL) {
    if (p->NO == no) {
      free_wp(p);
      return true;
    }
    p = p->next;
  }
  return false;
}

// 设置监视点
WP* set_watchpoint(char *e) {
  bool success;
  uint32_t val = expr(e, &success);
  if (!success) return NULL;

  WP *wp = new_wp();
  strncpy(wp->expr, e, 127);
  wp->expr[127] = '\0';
  wp->old_val = val;
  return wp;
}
