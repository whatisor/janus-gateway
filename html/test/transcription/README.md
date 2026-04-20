# Janus AudioBridge Transcription Test App

This is a test application for testing real-time transcription with the Janus AudioBridge plugin and ABMod module.

## Overview

This test app simulates a meeting scenario with 2 dummy peers that play audio files. It connects to Janus AudioBridge, enables transcription via ABMod, and allows you to control when audio is sent for transcription.

## Features

1. **Connects to local Janus server** (AudioBridge plugin)
2. **Toggle transcription on/off** via Load/Unload ABMod buttons
3. **2 dummy peers** that play audio files (peer1.wav and peer2.wav)
4. **Mute/Unmute controls** for each dummy peer to simulate speaking
5. **Real-time transcription display** showing partial and final transcriptions

## Setup

### 1. Audio Files Required

Place two audio files in this directory:
- `peer1.wav` - Audio file for dummy peer 1
- `peer2.wav` - Audio file for dummy peer 2

**Audio Format Requirements:**
- Format: WAV (recommended) or other browser-supported formats
- Sample rate: 16kHz or higher (24kHz recommended)
- Channels: Mono (1 channel)
- Duration: At least 5-10 seconds (will loop)

### 2. Starting the Test

1. Make sure Janus server is running
2. Open `index.html` in a web browser (Chrome/Firefox recommended)
3. Click "Start" to connect to Janus
4. Click "Load ABMod" to enable transcription
5. Use the "Unmute" buttons to start audio playback
6. Watch transcription appear in real-time

## Usage

### Starting a Test

1. **Start** button: Connects to Janus AudioBridge
2. **Load ABMod** button: Enables the transcription module
3. **Unmute (Peer 1/2)** buttons: Starts audio playback and sends to Janus

### Controls

- **Load ABMod / Unload ABMod**: Enable/disable transcription
- **Peer 1 / Peer 2 Unmute buttons**: Start audio playback
- **Peer 1 / Peer 2 Mute buttons**: Stop audio playback

### Transcription Display

- **Partial transcriptions** appear in blue with "(partial)" label
- **Final transcriptions** appear in normal text when speech ends
- Timestamps show when each transcription was generated
- Endpoint/provider behavior is controlled by `abmod_config` (provider defaults to AWS)

## Configuration

### Provider Configuration

Use `abmod_config` passed to `abmod_load` to select and tune providers:

- AWS (default): `{"provider":"aws", ...}`
- OpenAI: `{"provider":"openai", "openai_model":"gpt-4o-transcribe", ...}`
- Optional mixed-audio path: `{"enable_mix": true}` (per-user is still default)

## Troubleshooting

### Audio files not loading
- Ensure files are named exactly `peer1.wav` and `peer2.wav`
- Check browser console for CORS errors
- Verify audio file format is supported by browser

### No transcription appearing
- Verify ABMod is loaded (click "Load ABMod")
- Check that audio files are playing
- Ensure Janus server is configured with AudioBridge plugin
- Check browser console for WebSocket connection errors

### Transcription quality issues
- Confirm you are using the expected provider (`aws` vs `openai`) in `abmod_config`
- For OpenAI, tune options such as language/model/prompt in `abmod_config`
- Natural speech has pauses of 200-500ms between phrases; tune provider-specific turn behavior accordingly

## Files

- `index.html` - Main HTML interface
- `transcriptiontest.js` - JavaScript application logic
- `peer1.wav` - Audio file for dummy peer 1 (you provide)
- `peer2.wav` - Audio file for dummy peer 2 (you provide)

## Browser Compatibility

Tested with:
- Chrome/Edge 90+
- Firefox 88+
- Safari 14+

Requires WebRTC and Web Audio API support.

