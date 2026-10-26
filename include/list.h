#ifndef HONEY_BADGER_LIST_H
#define HONEY_BADGER_LIST_H

#define LIST_TYPE(T) T##List

#define LIST_DEFINE(T, N) typedef struct { T data[N]; T* end; } LIST_TYPE(T)

#define LIST_DECLARE(T, list) LIST_TYPE(T) list

#define LIST_INIT(list) ((list).end = (list).data)

#define LIST_SIZE(list) ((list).end - (list).data)

#define LIST_APPEND(list, item) (*(list).end++ = (item))

#define LIST_CLEAR(list) LIST_INIT(list)

#define LIST_START(list) (list).data

#define LIST_END(list) (list).end

#endif