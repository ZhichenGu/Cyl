/*
  ESP32 #2 — physical pushrods 5–8.
  Relay pins are deliberately unchanged: 5=(GPIO13,14), 6=(16,17),
  7=(18,19), 8=(25,26).  The common controller source keeps both boards
  on the same MQTT protocol and safety behaviour.
*/
#define PUSHROD_SECOND_BOARD
#include "../esp1234_hivemq/esp1234_hivemq.ino"
