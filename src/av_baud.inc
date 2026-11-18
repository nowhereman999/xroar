/* The socket is a byte stream. Model the independent receiver clock, switching
 * only after the peer's successful 38,400 LINKRATE reply has left the wire. */
static void av_uart_reply_byte(struct serial_jack *s, uint8_t b) {
	static const uint8_t prefix[] = {0x7e,5,0x2c,0,1,0xc2,0};
	unsigned i = s->rate_match;
	if (i < sizeof prefix) {
		s->rate_match = b == prefix[i] ? i+1 : b == prefix[0];
	} else if (i == sizeof prefix) {
		s->rate_token = b;
		++s->rate_match;
	} else {
		if (s->rate_token && (uint8_t)(5+0x2c+1+0xc2+s->rate_token+b)==0)
			s->rate_after_reply = true;
		s->rate_match = 0;
	}
}
