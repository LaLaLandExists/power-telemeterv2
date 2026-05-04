#pragma once

#ifdef PZEM_FAKE
/**
 * Fake PZEM + relay task for network-test builds (-D PZEM_FAKE).
 * Fills g_pzem with simulated readings every 500 ms so the LoRa TDMA path
 * can be exercised without real metering hardware attached.
 */
void fakePzemTask(void* params);
#endif
