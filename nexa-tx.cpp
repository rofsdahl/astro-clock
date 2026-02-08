#include "nexa-tx.h"

// System Nexa symbol bit patterns
static constexpr const char BITS_S[] = "10000000000";
static constexpr const char BITS_0[] = "10000010";
static constexpr const char BITS_1[] = "10100000";
static constexpr const char BITS_P[] = "10000000000000000000000000000000000000000";

NexaTx::NexaTx(uint8_t pin) : pin(pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}

const char* NexaTx::getBits(char symbol) {
  switch (symbol) {
    case 'S': return BITS_S;
    case '0': return BITS_0;
    case '1': return BITS_1;
    case 'P': return BITS_P;
    default:  return nullptr;
  }
}

void NexaTx::sendSymbol(char symbol) {
  const char *bits = getBits(symbol);
  if (bits == nullptr) return;

  unsigned long tNext = micros();
  while (*bits) {
    tNext += 250;
    digitalWrite(pin, (*bits == '1') ? HIGH : LOW);
    while ((long)(micros() - tNext) < 0);
    bits++;
  }
}

void NexaTx::transmit(uint32_t id, uint8_t unit, bool activation, uint8_t repetitions) {
  if (unit < 1 || unit > 4) return;

  for (uint8_t r = 0; r < repetitions; r++) {
    for (uint8_t frame = 0; frame < 5; frame++) {

      // S = Sync bit.
      sendSymbol('S');

      // U = Unique id, 26 bits. This is this code that the reciever "learns" to recognize. MSB first.
      uint32_t mask = 0x02000000UL;
      for (uint8_t i = 0; i < 26; i++, mask >>= 1) {
        sendSymbol((id & mask) ? '1' : '0');
      }

      // G = Group bit. All units = '0', one unit = '1'.
      sendSymbol('1');

      // A = Activation bit. On = '0', off = '1'.
      sendSymbol(activation ? '0' : '1');

      // C = Channel bits. Proove/Anslut = '00', Nexa = '11'.
      sendSymbol('1');
      sendSymbol('1');

      // I = Unit bits. Proove/Anslut: 1 = '00', 2 = '01', 3 = '10', 4 = '11'.
      //                Nexa:          1 = '11', 2 = '10', 3 = '01', 4 = '00'.
      switch (unit) {
        case 1: sendSymbol('1'); sendSymbol('1'); break;
        case 2: sendSymbol('1'); sendSymbol('0'); break;
        case 3: sendSymbol('0'); sendSymbol('1'); break;
        case 4: sendSymbol('0'); sendSymbol('0'); break;
      }

      // P = Pause bit.
      sendSymbol('P');
    }

    // Inter-transmission gap (35 ms)
    ets_delay_us(35000);
  }
}
