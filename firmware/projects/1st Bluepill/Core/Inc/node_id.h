/**
 * node_id.h - Which CAN node this board is.
 *
 * This is the ONLY file that differs between the three active Bluepill
 * projects. (A 4th project exists but is retired - see
 * firmware/bluepill_node/INTEGRATION.md.)
 *
 * TEMPORARILY REASSIGNED 2026-08-14 for the v2 bench test: this board was
 * the idle v1/v2 spare (NODE_ID 0) and is now standing in for the E-CVT/
 * Motor node (NODE_ID 3) so the hub's reassembler gets exercised by a real
 * 4th CAN transmitter instead of leaving node 3 hub-internal for the test -
 * see can_node_v2.h's header comment and HUB2_SIMULATE_NODE3 in
 * telemetry_hub_v2.h (must be 0 on the hub while this board is on the bus).
 * This board is no longer a working v1 fallback spare while wearing this
 * hat - set this back to 0 to restore that role once the bench test is done
 * or the real ODrive replaces this stand-in.
 */

#ifndef NODE_ID_H
#define NODE_ID_H

#define NODE_ID 3

#endif /* NODE_ID_H */
