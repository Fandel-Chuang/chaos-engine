/*
 * Quick test for ce_client_network protocol
 * Tests binary protocol encoding/decoding without a real server
 */
#include "runtime/ce_client_network.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    int failures = 0;

    /* Test 1: CeClientNet creation and disconnect (no server) */
    printf("Test 1: Create context (no server)...\n");
    CeClientNet* ctx = ce_client_net_connect("127.0.0.1", 9999);
    if (!ctx) {
        printf("  FAIL: ce_client_net_connect returned NULL\n");
        failures++;
    } else {
        printf("  PASS: ctx=%p, connected=%d\n", (void*)ctx, ctx->connected);
        /* Should be disconnected since no server */
        if (ctx->connected == 0) {
            printf("  PASS: Graceful fallback (not connected)\n");
        } else {
            printf("  WARN: Unexpectedly connected\n");
        }
        ce_client_net_disconnect(ctx);
    }

    /* Test 2: Entity array operations */
    printf("\nTest 2: Entity operations...\n");
    CeClientNet net;
    memset(&net, 0, sizeof(net));
    net.connected = 1;

    /* Simulate receiving a text UPDATE message */
    const char* msg1 = "UPDATE:42:1.5:2.5:3.5\n";
    memcpy(net.recv_buf, msg1, strlen(msg1));
    net.recv_offset = (int)strlen(msg1);

    int count = ce_client_net_poll(&net);
    if (count == 1 && net.entity_count == 1 && net.entities[0].entity_id == 42) {
        printf("  PASS: Parsed text UPDATE: entity 42 at (%.1f, %.1f, %.1f)\n",
               net.entities[0].x, net.entities[0].y, net.entities[0].z);
    } else {
        printf("  FAIL: Got count=%d, entities=%d, first id=%u\n",
               count, net.entity_count,
               net.entity_count > 0 ? net.entities[0].entity_id : 0);
        failures++;
    }

    /* Test 3: Multiple entities in text protocol */
    printf("\nTest 3: Multiple text updates...\n");
    net.entity_count = 0;
    net.recv_offset = 0;
    const char* msg2 = "UPDATE:100:10.0:20.0:30.0\nUPDATE:200:40.0:50.0:60.0\n";
    memcpy(net.recv_buf, msg2, strlen(msg2));
    net.recv_offset = (int)strlen(msg2);

    count = ce_client_net_poll(&net);
    if (count == 2 && net.entity_count == 2) {
        printf("  PASS: Parsed 2 entities\n");
    } else {
        printf("  FAIL: Got count=%d, entities=%d\n", count, net.entity_count);
        failures++;
    }

    /* Test 4: JOINED response */
    printf("\nTest 4: JOINED response...\n");
    net.entity_count = 0;
    net.recv_offset = 0;
    const char* msg3 = "JOINED:500\n";
    memcpy(net.recv_buf, msg3, strlen(msg3));
    net.recv_offset = (int)strlen(msg3);

    net.entity_id = 0;
    count = ce_client_net_poll(&net);
    if (count == 1 && net.entity_id == 500) {
        printf("  PASS: Parsed JOINED, entity_id=%u\n", net.entity_id);
    } else {
        printf("  FAIL: Got count=%d, entity_id=%u\n", count, net.entity_id);
        failures++;
    }

    /* Test 5: Partial message (incomplete line) */
    printf("\nTest 5: Partial message...\n");
    net.entity_count = 0;
    net.recv_offset = 0;
    const char* partial = "UPDATE:999:1.0:2.0";  /* no newline */
    memcpy(net.recv_buf, partial, strlen(partial));
    net.recv_offset = (int)strlen(partial);

    count = ce_client_net_poll(&net);
    if (count == 0 && net.entity_count == 0) {
        printf("  PASS: Partial message correctly ignored\n");
    } else {
        printf("  FAIL: Got count=%d, entities=%d (expected 0)\n", count, net.entity_count);
        failures++;
    }

    /* Test 6: Entity update (existing entity) */
    printf("\nTest 6: Update existing entity...\n");
    net.entity_count = 1;
    net.entities[0].entity_id = 42;
    net.entities[0].x = 0; net.entities[0].y = 0; net.entities[0].z = 0;
    net.recv_offset = 0;
    const char* update = "UPDATE:42:99.0:88.0:77.0\n";
    memcpy(net.recv_buf, update, strlen(update));
    net.recv_offset = (int)strlen(update);

    count = ce_client_net_poll(&net);
    if (count == 1 && net.entity_count == 1 &&
        net.entities[0].x == 99.0f && net.entities[0].y == 88.0f) {
        printf("  PASS: Entity 42 updated to (99.0, 88.0, 77.0)\n");
    } else {
        printf("  FAIL: entity_count=%d, pos=(%.1f, %.1f, %.1f)\n",
               net.entity_count,
               net.entities[0].x, net.entities[0].y, net.entities[0].z);
        failures++;
    }

    printf("\n========================================\n");
    if (failures == 0) {
        printf("  ALL TESTS PASSED\n");
    } else {
        printf("  %d TEST(S) FAILED\n", failures);
    }
    printf("========================================\n");
    return failures;
}
