#include "compatibility.h"

#if PG_VERSION_NUM < 130000
List *list_insert_nth(List *list, int pos, void *datum)
{
  unsigned i=0;
  ListCell *p, *prev, *new;

  if(!list && pos==0)
    return list_make1(datum);

  for(p = list->head, prev=NULL; p && i<pos; ++i, prev=p, p=p->next)
    ;

  Assert(i==pos);

  if(prev==NULL)
    return lcons(datum, list);

  new = (ListCell *) palloc(sizeof(ListCell));
  new->next=p;
  new->data.ptr_value=datum;
  prev->next=new;

  if(!p)
    list->tail=new;

  ++list->length;

  return list;
}
#endif
