#pragma once
#include <Arduino.h>

class NexaTx {
public:
  explicit NexaTx(uint8_t pin);

  // id: 26-bit Nexa unique ID
  // unit: 1–4
  // activation: true = ON, false = OFF
  // repetitions: number of full transmissions (default 4)
  void transmit(uint32_t id, uint8_t unit, bool activation, uint8_t repetitions = 4);

private:
  uint8_t pin;
  void sendSymbol(char symbol);
  const char* getBits(char symbol);
};
