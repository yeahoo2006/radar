#pragma once

#include <stdbool.h>
#include <stdint.h>

// Configuration bytes are the radar's raw 0x0012 and 0x0013 responses.
// Bit 0: detection query valid; bit 1: sensitivity query valid.
void ble_link_start(const uint8_t config[8], uint8_t valid_flags);

// targets contains count consecutive 5-byte LD2451 records, if count > 0.
void ble_link_publish(uint8_t count, uint8_t alarm, uint8_t ot1,
                      const uint8_t *targets);

// The GATT write callback queues one full 8-byte parameter set. The UART task
// takes and applies it, then reports the radar's readback via Config/Result.
bool ble_link_take_config_request(uint8_t desired[8], uint8_t *transaction_id);
void ble_link_finish_config(uint8_t transaction_id, uint8_t result_code,
                            uint8_t failed_stage, const uint8_t actual[8],
                            uint8_t valid_flags);
