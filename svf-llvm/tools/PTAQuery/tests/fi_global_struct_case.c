#include <stdlib.h>

struct arc {
    long cost;
    struct node *tail;
    struct node *head;
    struct arc *nextout;
    struct arc *nextin;
};

struct node {
    long potential;
    int  orientation;
    struct node *child;
    struct node *pred;
    struct arc  *basic_arc;
    struct arc  *firstout;
    struct arc  *firstin;
};

struct network {
    long         n;
    long         m;
    struct node *nodes;
    struct node *stop_nodes;
    struct arc  *arcs;
    struct arc  *stop_arcs;
};

struct network net;

long read_min(void)
{
    struct node *nodes = calloc(16, sizeof(struct node));
    net.nodes      = nodes;
    net.stop_nodes = nodes + 16;

    struct arc *arcs = calloc(16, sizeof(struct arc));
    net.arcs      = arcs;
    net.stop_arcs = arcs + 16;

    return 0;
}

long resize_prob(void)
{
    struct arc *new_arcs = realloc(net.arcs, 32 * sizeof(struct arc));
    if (new_arcs == NULL)
        return -1;
    net.arcs      = new_arcs;
    net.stop_arcs = new_arcs + 32;
    return 0;
}

void refresh_neighbour_lists(void)
{
    for (struct arc *arc = net.arcs; arc < net.stop_arcs; arc++) {
        arc->nextout = arc->tail->firstout;
        arc->tail->firstout = arc;

        arc->nextin = arc->head->firstin;
        arc->head->firstin = arc;
    }
}

int main(void)
{
    read_min();
    resize_prob();
    refresh_neighbour_lists();
    return 0;
}
