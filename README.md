# AzerothCore Module: LLM Chat for Playerbots

Local LLM-powered chat for [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) via [Ollama](https://ollama.com). Bots reply to whispers (and optionally say) using a local model like `gemma4:latest` — no cloud API, works offline. Ideal for solo/local servers.

## Features

- **Whisper + Say** via Ollama `/api/chat` (or `/api/generate`)
- Per-bot conversation history (`LLMHistorySize`)
- 100% hit rate for LLM channels (bypasses random reply chance)
- Per-bot rate limit (`LLMRateLimitPerBotMs`)
- Fallback text (`my brain hurts...`) on timeout/error
- Single worker thread, queued requests (`LLMMaxQueue`), async delivery on world thread

## Requirements

- AzerothCore (WotLK 3.3.5a) with `mod-playerbots` built (`-DMODULES=static`)
- Ollama running locally: https://ollama.com
- Model pulled: `ollama pull gemma4:latest` (or `gemma4-64k:latest`, `qwen`, etc.)
- `gemma4` reports capabilities `["completion","tools","thinking"]` — `"think":false` is sent

## Install

1. **Ollama**
```bash
curl -fsSL https://ollama.com/install.sh | sh
ollama serve &
ollama pull gemma4:latest
curl http://localhost:11434/api/tags  # verify
```

2. **Module**

This repo is a fork of `mod-playerbots` with LLM patches. Copy to your AzerothCore modules:

```bash
cp -r azerothcore-module-LLM-chat /path/to/azerothcore/modules/mod-playerbots
# or clone directly as mod-playerbots
```

Or apply the three patched files to an existing `mod-playerbots`:
- `src/Mgr/Ollama/OllamaChatService.h` / `.cpp` (new)
- `src/Bot/PlayerbotAI.cpp` (SMSG_MESSAGECHAT whisper bypass)
- `src/Script/Playerbots.cpp` (whisper → ChatReplyDo)
- `src/PlayerbotAIConfig.h` / `.cpp` + `conf/playerbots.conf.dist` (LLM options)

3. **Build**
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/azeroth-server -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMODULES=static -DSCRIPTS=static
cmake --build . -j$(nproc) --target worldserver
cmake --install .
cp $HOME/azeroth-server/bin/worldserver /path/to/wowserver/server/bin/worldserver
```

## Configuration

In `etc/modules/playerbots.conf` (the one under your **install prefix** `CMAKE_INSTALL_PREFIX/etc/modules/`, not just `wowserver/server/etc`):

```ini
AiPlayerbot.LLMEnabled = 1
AiPlayerbot.LLMUrl = "http://localhost:11434"
AiPlayerbot.LLMModel = "gemma4:latest"
AiPlayerbot.LLMApi = "chat"  # "chat" = POST /api/chat, "generate" = /api/generate
AiPlayerbot.LLMTimeoutMs = 60000  # 60s for first model load (then 5-10s is fine)
AiPlayerbot.LLMMaxTokens = 80
AiPlayerbot.LLMTemperature = 0.8
AiPlayerbot.LLMSystemPrompt = "You are a World of Warcraft character. You are helpful, terse, in-character, stay in lore, 1-2 sentences, max 200 characters. Never mention you are AI."
AiPlayerbot.LLMHistorySize = 5
AiPlayerbot.LLMFallbackText = "my brain hurts..."
AiPlayerbot.LLMRateLimitPerBotMs = 5000
AiPlayerbot.LLMMaxQueue = 100
AiPlayerbot.LLMMaxResponseChars = 255
AiPlayerbot.LLMEnabledForWhisper = 1
AiPlayerbot.LLMEnabledForSay = 1
```

Restart worldserver.

## Usage

- **Whisper:** `/w <BotName> hello there` — bot replies via LLM (works even if bot is idle/off-zone)
- **Say:** `/say hello everyone` near bots — bots in /say range reply (requires `LLMEnabledForSay=1`)

First whisper after restart takes 10–30s (Ollama loads ~9.6GB model), subsequent replies are 1–3s.

Check `Playerbots.log` / `worldserver.log`:
```
[Ollama] Initializing LLM: url=http://localhost:11434 ... timeout=60000ms
[Ollama] 1234ms bot=Thoren reply='...' (model=gemma4:latest)
```

## How it works

- `Player::Whisper` → `PlayerbotsPlayerScript::OnPlayerCanUseChat` (whisper) → `HandleCommand` + `ChatReplyAction::ChatReplyDo`
- `WorldSession::SendPacket(SMSG_MESSAGECHAT)` → `OnPlayerbotPacketSent` → `PlayerbotAI::HandleBotOutgoingPacket` (say/yell/whisper) → `QueueChatResponse` → `ChatReplyDo`
- `ChatReplyDo` checks blocklist (`noReplyMsgs`) and special handlers (LFG/WTB/thunderfury), then `OllamaChatService::ShouldUseLLM` / `IsRateLimited` → `EnqueueRequest` → worker thread `HttpPost` to Ollama → `ProcessCompleted` (world thread) → `ai->Whisper` / `ai->Say`

Rate limit is per-bot, stamped only on successful enqueue.

## Troubleshooting

- **No reply, no Ollama log:** check `LLMEnabled=1`, whisper a bot by exact name, ensure Ollama is up (`curl http://localhost:11434/api/tags`), check `etc/modules/playerbots.conf` under install prefix is the one edited (not just `wowserver/server/etc`).
- **Fallback "my brain hurts...":** Ollama timeout or model not loaded — increase `LLMTimeoutMs` to 60000 for first load, ensure `ollama pull gemma4:latest` succeeded, check worker logs for `No response` / `Empty content`.
- **Say floods:** set `LLMEnabledForSay=0` or keep `LLMRateLimitPerBotMs=5000` and `LLMMaxQueue=100`.
- **Idle bots don't answer say:** say range (~25yd) is within `BotActiveAloneForceWhenInRadius=150` so say works; whispers bypass `AllowActivity` regardless of distance.

## Security

No API keys. Ollama is local. Do not commit `playerbots.conf` with secrets.

## License

Same as mod-playerbots / AzerothCore: AGPL-3.0. See `LICENSE`.
