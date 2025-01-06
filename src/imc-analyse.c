#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*  Output a list of type-frequency pairs from a packet log input.
 *  Currently assumes everything is protocol version 2 (fixme: put in a hook
 *  in imc.c:forward() to pass all packets received to a higher layer)
 */

typedef struct _nametag {
  char *name;
  int freq;
  struct _nametag *next;
} nametag;

nametag *taglist;

/* add a name to taglist, or increase it's frequency */
void addname(const char *name)
{
  nametag *p;

  for (p=taglist; p; p=p->next)
    if (!strcasecmp(p->name, name))
      break;

  if (!p)
  {
    /* allocate new entry */

    p=malloc(sizeof(nametag));
    p->next=taglist;
    p->name=strdup(name);
    p->freq=1;
    taglist=p;
  }
  else
  {
    /* increment freq on existing entry */
    p->freq++;
  }
}

/* Parse a log line */

void parseline(const char *line)
{
  char type[100];

  /*  Format: mudname[desc] dirchar packet
   *
   *  eg:     BV[0] > ....
   *
   *  We want the 4th field in the packet data (packet type)
   */

  sscanf(line, "%*s %*s %*s %*s %*s %99s", type);
  if (type[0])
    addname(type);
}

/* dump nametag list and free it */

void dump(void)
{
  nametag *p, *p_next;

  for (p=taglist; p; p=p_next)
  {
    p_next=p->next;
    printf("%s %d\n", p->name, p->freq);
    free(p->name);
    free(p);
  }
}

int main(int argc, char *argv[])
{
  char line[16000];

  /* read lines from stdin and process them until we run out */

  fgets(line, 16000, stdin);
  while(!feof(stdin))
  {
    parseline(line);
    fgets(line, 16000, stdin);
  }

  /* dump results */

  dump();

  return 0;
}
