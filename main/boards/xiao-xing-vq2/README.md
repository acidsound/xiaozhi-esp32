# Xiao Xing VQ2

Board definition for the Xiao Xing VQ2 ESP32-S3-N16R8 robot board.

Confirmed hardware:

- ESP32-S3-N16R8 module, expected 16 MB flash and 8 MB PSRAM.
- SH1106 128x64 I2C OLED, SDA GPIO41, SCL GPIO42.
- I2S speaker: BCLK GPIO15, LRCK GPIO16, DOUT GPIO7.
- I2S microphone: SCK GPIO5, WS GPIO4, DIN GPIO6.
- Four populated servo outputs: FL GPIO17, FR GPIO13, BL GPIO18, BR GPIO14.
- Optional factory tail servo slot on GPIO12 is not populated on the tested unit.
- Two lower WS2812 RGB LEDs on GPIO8.

The runtime implementation mirrors the ESP-HI dog and light controls while
keeping the VQ2 display and I2S audio wiring.

## Brave Search

This board exposes a device-side MCP search tool through `self.web.search`.
The tool is designed for Korean voice prompts such as "검색해줘" or
"알아봐줘" and uses Brave Search from the device firmware without a separate
server.

Configure Search from the board web UI:

1. Connect the device to Wi-Fi.
2. Ask the device for its IP address, or check the serial log.
3. Open `http://<device-ip>/`.
4. Go to `Settings` > `Web Search`.

The settings page has two Brave API key fields:

- `Web Search API key`: primary key for normal Brave Web Search. Use a
  deprecated Free Search key here if the account still has one.
- `LLM Context API key`: optional key for Brave LLM Context. For a
  new Brave Search plan, this can be the same key as the Web Search key.

Search order:

1. Simple prompts such as "검색해줘", "알아봐줘", "오늘 뉴스 알려줘", and
   "요즘 뭐가 핫해?" use the Web Search key.
2. Deeper prompts such as "조사해줘", "자세히 알아봐줘", "근거까지 찾아봐줘",
   "출처 내용을 보고 요약해줘", "본문 기준으로 알려줘", "비교해서 알려줘",
   and "왜 그런지 분석해줘" use the LLM Context key when the checkbox is
   enabled.
3. If a simple Web Search request returns a monthly quota exhaustion signal
   (`rate_limited` with the monthly `X-RateLimit-Remaining` value at `0`), and
   the LLM Context checkbox is enabled, the firmware retries with the LLM
   Context key.
4. Per-second rate limits, network errors, and invalid Web Search keys do not
   automatically switch to the LLM Context key.

To use LLM Context, enable a Brave Search plan that includes LLM Context in the
Brave API dashboard, then save that key in `LLM Context API key` and enable
`Allow LLM Context for detailed research and quota fallback`. If the same key
has both Web Search and LLM Context access, enter the same key in both fields.
If the account has an older Free Search key and a newer Search plan key, keep
them separated so simple searches use the free Web Search quota first while
research-style prompts can use LLM Context directly.

Secret handling is enforced by the firmware interface, not by prompt wording.
The web settings API returns only `web_configured`, `llm_context_configured`,
and `use_llm_context` booleans. The MCP config-status tool checks only NVS
string length and returns the same booleans. The search tool reads the key only
inside the device to set Brave's `X-Subscription-Token` request header and never
adds the key or raw upstream error bodies to MCP result JSON or logs. This does
not protect against physical flash extraction on an unencrypted device, but it
keeps the key out of the external LLM/server data path.

Brave's dashboard and rate-limit headers are the authoritative source for
usage because one API key may be shared by multiple clients. The firmware does
not maintain a local monthly counter. `X-RateLimit-Remaining` is read from each
Brave API response, so no separate quota-only request is needed. When the
monthly remaining quota is 1% or less, the tool result includes a short
`quota_warning_message` so the assistant can mention the remaining request
count after answering. To avoid charges beyond the included monthly credits, set
a monthly credit limit in the Brave account dashboard, for example `$5` when
using the included `$5` monthly credits.
