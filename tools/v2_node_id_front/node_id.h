/* node_id.h for the v2 Front-node host test build (tools/test_can_node_v2.c,
   via tools/test_can_node_v2.py). Mirrors the shape of the real per-project
   Core/Inc/node_id.h files under firmware/projects/ and of
   tools/hal_shim/node_id.h - just a different value, in its own directory,
   so it can sit on the include path ahead of tools/hal_shim/node_id.h
   (which is fixed at NODE_ID 0 for the v1 host tests and is not touched by
   this file). NODE_ID is a compile-time constant, so testing both Front and
   Rear means two separate builds, each with its own node_id.h - see
   tools/test_can_node_v2.py. */

#ifndef NODE_ID_H
#define NODE_ID_H

#define NODE_ID 1

#endif
