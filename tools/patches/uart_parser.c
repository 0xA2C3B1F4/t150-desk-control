/* T150 application RX parser replacement. DMA's producer byte records receive
 * progress, so a split frame must keep its decode state while the consumer
 * advances. The four state bytes live after RS01's 28-byte state.
 * A frame cut directly after FE consumes the next FA as data. Senders precede
 * each frame with an extra FA, which ends that escape; FA never dispatches. */
typedef unsigned char u8;
typedef unsigned int u32;

enum { IDLE, FRAME, ESCAPED, DISCARD, DISCARD_ESCAPED };
enum { PARSER_MAGIC = 0xa6 };
typedef struct {
  u8 mode, count, check, magic;
} ParserState;
#define P (*(volatile ParserState*)0x20000fecu)

void rs_parser_init(void) {
  P.magic = 0;
  P.mode = IDLE;
  P.count = 0;
  P.check = 0;
  P.magic = PARSER_MAGIC;
}

static u8 next(u8 index) {
  return (u8)(index + 1u);
}

/* Stock entry ABI: RX descriptor, decoded-request descriptor, error bytes.
 * A positive return is the decoded length including the XOR byte. */
u32 rs_parser(volatile u8* rx, volatile u32* request, volatile u8* error) {
  if (P.magic != PARSER_MAGIC || P.mode > DISCARD_ESCAPED || P.count > 127)
    rs_parser_init();
  volatile u8* ring = (volatile u8*)(*(volatile u32*)rx);
  volatile u8* out = (volatile u8*)(*request);
  const u8 producer = rx[5];
  u8 cursor = rx[4];

  while (cursor != producer) {
    const u8 value = ring[cursor];
    cursor = next(cursor);
    rx[4] = cursor;

    if (P.mode == IDLE) {
      if (value == 0xfau) {
        P.mode = FRAME;
        P.count = P.check = 0;
      }
      continue;
    }
    if (P.mode == DISCARD_ESCAPED) {
      P.mode = DISCARD;
      continue;
    }
    if (P.mode == DISCARD) {
      if (value == 0xfeu)
        P.mode = DISCARD_ESCAPED;
      else if (value == 0xfau) {
        P.mode = FRAME;
        P.count = P.check = 0;
      } else if (value == 0xfdu)
        P.mode = IDLE;
      continue;
    }
    if (P.mode == FRAME) {
      if (value == 0xfau) {
        P.count = P.check = 0;
        continue;
      }
      if (value == 0xfeu) {
        P.mode = ESCAPED;
        continue;
      }
      if (value == 0xfdu) {
        const u8 count = P.count;
        const u8 check = P.check;
        P.mode = IDLE;
        if (count == 0)
          return 0;
        if (check != 0) {
          error[1] = 1;
          error[0] = 1;
          return 0;
        }
        /* The stock 17 handler ignores length: selector 1 reads byte
         * 3 and the others byte 2. Admit only those exact lengths so
         * the XOR byte, or a frame merged after a cut FE escape,
         * cannot supply a button action. */
        if (out[0] == 0x17u && count != (out[1] == 1u ? 5u : 4u)) {
          error[1] = 2;
          error[0] = 1;
          return 0;
        }
        return count;
      }
    } else {
      /* ESCAPED: FA, FD and FE are all literal data here. */
      P.mode = FRAME;
    }
    if (P.count >= 127) {
      P.mode = DISCARD;
      error[1] = 2;
      error[0] = 1;
      return 0;
    }
    out[P.count] = value;
    P.count++;
    P.check ^= value;
  }
  return 0;
}
