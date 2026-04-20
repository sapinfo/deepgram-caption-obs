/*
 * Deepgram Captions for OBS
 * Real-time speech-to-text captions using Deepgram Nova-3 API
 *
 * Audio capture → Deepgram WebSocket → Real-time caption display
 */

#include <obs-module.h>
#include <media-io/audio-resampler.h>
#include <plugin-support.h>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <thread>
#include <chrono>
#include <sstream>
#include <cctype>

using json = nlohmann::json;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// ─── Source data structure ───
struct deepgram_caption_data {
	obs_source_t *source;
	obs_source_t *text_source;

	// Hotkey
	obs_hotkey_id hotkey_id{OBS_INVALID_HOTKEY_ID};

	// WebSocket
	std::unique_ptr<ix::WebSocket> websocket;
	std::atomic<bool> connected{false};
	std::atomic<bool> captioning{false};
	std::atomic<bool> stopping{false};

	// KeepAlive thread
	std::thread keepalive_thread;
	std::atomic<bool> keepalive_running{false};

	// Audio capture
	obs_source_t *audio_source{nullptr};
	std::string audio_source_name;

	// Caption state
	std::mutex text_mutex;
	std::string final_buffer;
	std::string partial_text;
	int utterance_count{0};

	// Settings
	int font_size{48};
	std::string font_face{"Apple SD Gothic Neo"};
	std::string font_style{"Regular"};
	int font_flags{0};
	std::string api_key;
	std::string language{"ko"};
	std::string model{"nova-3"};
	bool smart_format{true};
	bool punctuate{true};
	bool interim_results{true};
	int endpointing_ms{300};
	int utterance_end_ms{1000};

	// Nova-3 features
	std::string keyterms;      // newline-separated list
	bool diarize{false};
	bool profanity_filter{false};
	std::string redact_mode;   // "off", "numbers", "pci", "ssn", "all"
	bool numerals{false};
	bool filler_words{false};
	bool detect_entities{false};
	bool mip_opt_out{false};

	// Audio resampling (OBS project SR -> 16kHz mono s16le)
	audio_resampler_t *resampler{nullptr};

	// Diarization state (speaker label tracking across interim updates)
	int last_speaker{-1};

	// Text style
	uint32_t color1{0xFFFFFFFF}; // ABGR (OBS internal format)
	uint32_t color2{0xFFFFFFFF};
	bool outline{false};
	bool drop_shadow{false};
	int custom_width{0};
	bool word_wrap{false};
};

// ─── URL encode helper (RFC 3986 unreserved chars only) ───
static std::string url_encode(const std::string &value)
{
	std::string out;
	out.reserve(value.size() * 3);
	for (unsigned char c : value) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			char buf[4];
			snprintf(buf, sizeof(buf), "%%%02X", c);
			out += buf;
		}
	}
	return out;
}

// ─── Trim whitespace (including \r for CRLF inputs) ───
static void trim_inplace(std::string &s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
		s.erase(s.begin());
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
		s.pop_back();
}

// ─── Update text display ───
static void update_text_display(deepgram_caption_data *data, const char *text)
{
	if (!data->text_source)
		return;

	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", data->font_face.c_str());
	obs_data_set_int(font, "size", data->font_size);
	obs_data_set_string(font, "style", data->font_style.c_str());
	obs_data_set_int(font, "flags", data->font_flags);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "text", text);
	obs_data_set_obj(s, "font", font);

#ifdef _WIN32
	// text_gdiplus properties
	obs_data_set_int(s, "color", data->color1);
	obs_data_set_int(s, "opacity", 100);
	obs_data_set_bool(s, "outline", data->outline);
	obs_data_set_int(s, "outline_size", 4);
	obs_data_set_int(s, "outline_color", 0x000000);
	obs_data_set_int(s, "outline_opacity", 100);
	if (data->custom_width > 0) {
		obs_data_set_bool(s, "extents", true);
		obs_data_set_int(s, "extents_cx", data->custom_width);
		obs_data_set_int(s, "extents_cy", 0);
		obs_data_set_bool(s, "extents_wrap", data->word_wrap);
	} else {
		obs_data_set_bool(s, "extents", false);
	}
#else
	// text_ft2_source_v2 properties
	obs_data_set_int(s, "color1", data->color1);
	obs_data_set_int(s, "color2", data->color2);
	obs_data_set_bool(s, "outline", data->outline);
	obs_data_set_bool(s, "drop_shadow", data->drop_shadow);
	obs_data_set_int(s, "custom_width", data->custom_width);
	obs_data_set_bool(s, "word_wrap", data->word_wrap);
#endif

	obs_source_update(data->text_source, s);

	obs_data_release(font);
	obs_data_release(s);
}

// ─── Audio capture callback ───
// Uses OBS's audio_resampler to convert the project's actual SR/layout
// to 16kHz mono int16 with proper anti-aliasing, instead of naive 3:1 decimation.
static void audio_capture_callback(void *param, obs_source_t *, const struct audio_data *audio,
				   bool muted)
{
	auto *data = static_cast<deepgram_caption_data *>(param);

	if (!data->captioning || !data->connected || !data->websocket || muted)
		return;
	if (!data->resampler || !audio->data[0] || audio->frames == 0)
		return;

	uint8_t *output[MAX_AV_PLANES] = {0};
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;
	bool ok = audio_resampler_resample(data->resampler, output, &out_frames, &ts_offset,
					   reinterpret_cast<const uint8_t *const *>(audio->data),
					   audio->frames);
	if (!ok || out_frames == 0 || !output[0])
		return;

	data->websocket->sendBinary(std::string(reinterpret_cast<const char *>(output[0]),
						out_frames * sizeof(int16_t)));
}

// ─── Handle Deepgram response message ───
static void handle_deepgram_message(deepgram_caption_data *data, const std::string &msg_str)
{
	try {
		json resp = json::parse(msg_str);

		std::string msg_type = resp.value("type", "");

		// Metadata message at connection start
		if (msg_type == "Metadata") {
			obs_log(LOG_INFO, "Deepgram session started (request_id: %s)",
				resp.value("request_id", "").c_str());
			return;
		}

		// UtteranceEnd: speaker finished speaking
		if (msg_type == "UtteranceEnd") {
			std::lock_guard<std::mutex> lock(data->text_mutex);
			std::string line = data->final_buffer;
			while (!line.empty() && line.front() == ' ')
				line.erase(line.begin());
			while (!line.empty() && line.back() == ' ')
				line.pop_back();

			if (!line.empty()) {
				data->utterance_count++;
				obs_log(LOG_INFO, "[Utterance %d] %s", data->utterance_count, line.c_str());
			}

			data->final_buffer.clear();
			data->partial_text.clear();
			data->last_speaker = -1;
			update_text_display(data, "");
			return;
		}

		// SpeechStarted event
		if (msg_type == "SpeechStarted") {
			return;
		}

		// Error response
		if (msg_type == "Error" || resp.contains("err_code")) {
			std::string err = resp.value("err_msg", resp.value("message", "Unknown error"));
			obs_log(LOG_ERROR, "Deepgram error: %s", err.c_str());
			update_text_display(data, ("Error: " + err).c_str());
			return;
		}

		// Main transcription results
		if (msg_type != "Results")
			return;

		auto channel = resp.value("channel", json::object());
		auto alternatives = channel.value("alternatives", json::array());
		if (alternatives.empty())
			return;

		std::string transcript = alternatives[0].value("transcript", "");
		bool is_final = resp.value("is_final", false);
		bool speech_final = resp.value("speech_final", false);

		// Diarization: rebuild transcript with [S1]/[S2] labels when speaker changes
		if (data->diarize) {
			auto words = alternatives[0].value("words", json::array());
			if (!words.empty()) {
				std::string labeled;
				int prev = data->last_speaker;
				for (auto &w : words) {
					int sp = w.value("speaker", 0);
					std::string token = w.value("punctuated_word",
								   w.value("word", std::string()));
					if (token.empty())
						continue;
					if (sp != prev) {
						if (!labeled.empty())
							labeled += " ";
						labeled += "[S" + std::to_string(sp + 1) + "] ";
						prev = sp;
					} else if (!labeled.empty()) {
						labeled += " ";
					}
					labeled += token;
				}
				if (!labeled.empty()) {
					transcript = labeled;
					if (is_final)
						data->last_speaker = prev;
				}
			}
		}

		std::lock_guard<std::mutex> lock(data->text_mutex);

		if (is_final && !transcript.empty()) {
			// Final result: append to buffer
			if (!data->final_buffer.empty())
				data->final_buffer += " ";
			data->final_buffer += transcript;

			if (speech_final) {
				// Speaker finished: log and clear
				std::string line = data->final_buffer;
				while (!line.empty() && line.front() == ' ')
					line.erase(line.begin());
				while (!line.empty() && line.back() == ' ')
					line.pop_back();

				if (!line.empty()) {
					data->utterance_count++;
					obs_log(LOG_INFO, "[Utterance %d] %s", data->utterance_count,
						line.c_str());
				}

				// Display final result briefly, then clear
				update_text_display(data, data->final_buffer.c_str());
				data->final_buffer.clear();
				data->partial_text.clear();
			} else {
				// Final but not speech_final: show accumulated text
				update_text_display(data, data->final_buffer.c_str());
			}
		} else if (!is_final && !transcript.empty()) {
			// Interim result: show current buffer + partial
			std::string display = data->final_buffer;
			if (!display.empty())
				display += " ";
			display += transcript;

			while (!display.empty() && display.front() == ' ')
				display.erase(display.begin());

			data->partial_text = transcript;
			update_text_display(data, display.c_str());
		}

	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "JSON parse error: %s", e.what());
	}
}

// ─── KeepAlive thread ───
static void keepalive_thread_func(deepgram_caption_data *data)
{
	while (data->keepalive_running) {
		// Sleep 5 seconds in 100ms increments for responsive shutdown
		for (int i = 0; i < 50 && data->keepalive_running; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (data->keepalive_running && data->connected && data->websocket) {
			data->websocket->send(R"({"type":"KeepAlive"})");
		}
	}
}

// ─── Stop captioning ───
static void stop_captioning(deepgram_caption_data *data)
{
	if (!data->captioning)
		return;

	data->stopping = true;
	data->captioning = false;
	data->connected = false;

	// Stop keepalive thread
	data->keepalive_running = false;
	if (data->keepalive_thread.joinable())
		data->keepalive_thread.join();

	if (data->audio_source) {
		obs_source_remove_audio_capture_callback(data->audio_source, audio_capture_callback,
							 data);
		obs_source_release(data->audio_source);
		data->audio_source = nullptr;
	}

	if (data->websocket) {
		// Send CloseStream before stopping
		data->websocket->send(R"({"type":"CloseStream"})");
		data->websocket->stop();
		data->websocket.reset();
	}

	if (data->resampler) {
		audio_resampler_destroy(data->resampler);
		data->resampler = nullptr;
	}

	data->last_speaker = -1;
	data->stopping = false;
	update_text_display(data, "Deepgram Captions Ready!");
	obs_log(LOG_INFO, "Captioning stopped");
}

// ─── Build Deepgram WebSocket URL with query parameters ───
static std::string build_deepgram_url(deepgram_caption_data *data)
{
	std::string url = "wss://api.deepgram.com/v1/listen";
	url += "?model=" + data->model;
	url += "&language=" + data->language;
	url += "&encoding=linear16";
	url += "&sample_rate=16000";
	url += "&channels=1";
	url += "&endpointing=" + std::to_string(data->endpointing_ms);

	if (data->punctuate)
		url += "&punctuate=true";
	if (data->smart_format)
		url += "&smart_format=true";
	if (data->interim_results)
		url += "&interim_results=true";

	// Enable utterance end detection for clearing captions
	url += "&utterance_end_ms=" + std::to_string(data->utterance_end_ms);
	url += "&vad_events=true";

	// Nova-3 / accuracy & formatting features
	if (data->diarize)
		url += "&diarize=true";
	if (data->profanity_filter)
		url += "&profanity_filter=true";
	if (!data->redact_mode.empty() && data->redact_mode != "off") {
		if (data->redact_mode == "all") {
			url += "&redact=numbers&redact=pci&redact=ssn";
		} else {
			url += "&redact=" + data->redact_mode;
		}
	}
	if (data->numerals)
		url += "&numerals=true";
	if (data->filler_words)
		url += "&filler_words=true";
	if (data->detect_entities)
		url += "&detect_entities=true";
	if (data->mip_opt_out)
		url += "&mip_opt_out=true";

	// Keyterm prompting (Nova-3): newline-separated list, each line -> &keyterm=...
	if (!data->keyterms.empty()) {
		std::istringstream iss(data->keyterms);
		std::string line;
		while (std::getline(iss, line)) {
			trim_inplace(line);
			if (!line.empty())
				url += "&keyterm=" + url_encode(line);
		}
	}

	// Tag requests for dashboard filtering
	url += "&tag=obs-plugin-" + std::string(PLUGIN_VERSION);

	return url;
}

// ─── Start captioning ───
static void start_captioning(deepgram_caption_data *data)
{
	if (data->captioning)
		stop_captioning(data);

	if (data->api_key.empty()) {
		update_text_display(data, "[Enter API Key first!]");
		return;
	}

	obs_source_t *audio_src = obs_get_source_by_name(data->audio_source_name.c_str());
	if (!audio_src) {
		update_text_display(data, "[Select Audio Source!]");
		return;
	}

	update_text_display(data, "Connecting...");

	{
		std::lock_guard<std::mutex> lock(data->text_mutex);
		data->final_buffer.clear();
		data->partial_text.clear();
		data->utterance_count = 0;
		data->last_speaker = -1;
	}

	// 0. Create resampler matching OBS project audio -> 16kHz mono s16le
	struct obs_audio_info oai = {};
	if (!obs_get_audio_info(&oai)) {
		oai.samples_per_sec = 48000;
		oai.speakers = SPEAKERS_STEREO;
	}

	struct resample_info src_info = {};
	src_info.samples_per_sec = oai.samples_per_sec;
	src_info.format = AUDIO_FORMAT_FLOAT_PLANAR;
	src_info.speakers = oai.speakers;

	struct resample_info dst_info = {};
	dst_info.samples_per_sec = 16000;
	dst_info.format = AUDIO_FORMAT_16BIT;
	dst_info.speakers = SPEAKERS_MONO;

	if (data->resampler) {
		audio_resampler_destroy(data->resampler);
		data->resampler = nullptr;
	}
	data->resampler = audio_resampler_create(&dst_info, &src_info);
	if (!data->resampler) {
		obs_log(LOG_ERROR, "Failed to create audio resampler (%u Hz -> 16 kHz)",
			oai.samples_per_sec);
		update_text_display(data, "Error: audio resampler init failed");
		obs_source_release(audio_src);
		return;
	}
	obs_log(LOG_INFO, "Audio resampler: %u Hz (%d ch) -> 16000 Hz mono",
		oai.samples_per_sec, (int)oai.speakers);

	// 1. Setup WebSocket
	data->websocket = std::make_unique<ix::WebSocket>();
	data->websocket->setUrl(build_deepgram_url(data));

	// Set authorization header
	ix::WebSocketHttpHeaders headers;
	headers["Authorization"] = "Token " + data->api_key;
	data->websocket->setExtraHeaders(headers);

	// Auto-reconnect: retry after 3s on disconnect
	data->websocket->enableAutomaticReconnection();
	data->websocket->setMinWaitBetweenReconnectionRetries(3000);
	data->websocket->setMaxWaitBetweenReconnectionRetries(30000);

	data->websocket->setOnMessageCallback([data](const ix::WebSocketMessagePtr &msg) {
		switch (msg->type) {
		case ix::WebSocketMessageType::Open: {
			obs_log(LOG_INFO, "Deepgram WebSocket connected");
			data->connected = true;
			update_text_display(data, "Listening...");
			break;
		}

		case ix::WebSocketMessageType::Message:
			handle_deepgram_message(data, msg->str);
			break;

		case ix::WebSocketMessageType::Error:
			obs_log(LOG_ERROR, "WS error: %s (status=%d)", msg->errorInfo.reason.c_str(),
				msg->errorInfo.http_status);
			data->connected = false;
			update_text_display(data, ("Error: " + msg->errorInfo.reason).c_str());
			break;

		case ix::WebSocketMessageType::Close:
			obs_log(LOG_INFO, "WS closed (code=%d, reason=%s)", msg->closeInfo.code,
				msg->closeInfo.reason.c_str());
			data->connected = false;
			if (!data->stopping && data->captioning) {
				std::string reason = msg->closeInfo.reason.empty()
							    ? "Connection closed"
							    : msg->closeInfo.reason;
				update_text_display(data, reason.c_str());
			}
			break;

		default:
			break;
		}
	});

	// 2. Register audio capture (connected=false so no data sent yet)
	data->audio_source = audio_src;
	obs_source_add_audio_capture_callback(audio_src, audio_capture_callback, data);

	// 3. Enable captioning and start WebSocket
	data->captioning = true;
	data->websocket->start();

	// 4. Start KeepAlive thread (sends every 5 seconds)
	data->keepalive_running = true;
	data->keepalive_thread = std::thread(keepalive_thread_func, data);

	obs_log(LOG_INFO, "Caption started, waiting for connection...");
}

// ─── Test connection (no audio) ───
static void test_connection(deepgram_caption_data *data)
{
	if (data->websocket) {
		data->websocket->stop();
		data->websocket.reset();
		data->connected = false;
	}

	if (data->api_key.empty()) {
		update_text_display(data, "[Enter API Key first!]");
		return;
	}

	update_text_display(data, "Testing connection...");

	data->websocket = std::make_unique<ix::WebSocket>();
	data->websocket->setUrl(build_deepgram_url(data));

	ix::WebSocketHttpHeaders headers;
	headers["Authorization"] = "Token " + data->api_key;
	data->websocket->setExtraHeaders(headers);
	data->websocket->disableAutomaticReconnection();

	data->websocket->setOnMessageCallback([data](const ix::WebSocketMessagePtr &msg) {
		switch (msg->type) {
		case ix::WebSocketMessageType::Open: {
			data->connected = true;
			update_text_display(data, "Connected OK!");
			obs_log(LOG_INFO, "Test connection: OK");

			// Close immediately after successful test.
			// stop() joins the WebSocket run thread. This callback
			// runs on that thread, so calling stop() here triggers
			// self-join -> system_error -> terminate -> SIGABRT.
			// Delegate to a detached thread so the callback can
			// return first and the run thread exits naturally.
			std::string close_msg = "{\"type\":\"CloseStream\"}";
			data->websocket->sendText(close_msg);
			ix::WebSocket *ws = data->websocket.get();
			std::thread([ws]() { ws->stop(); }).detach();
			break;
		}
		case ix::WebSocketMessageType::Message: {
			try {
				json resp = json::parse(msg->str);
				if (resp.contains("err_code"))
					update_text_display(
						data,
						("Error: " + resp.value("err_msg", "Unknown")).c_str());
				else if (resp.value("type", "") == "Metadata")
					update_text_display(data, "Connected! Ready.");
			} catch (...) {
			}
			break;
		}
		case ix::WebSocketMessageType::Error:
			update_text_display(data, ("Error: " + msg->errorInfo.reason).c_str());
			break;
		case ix::WebSocketMessageType::Close:
			if (!data->stopping)
				update_text_display(data, "Test: Disconnected");
			data->connected = false;
			break;
		default:
			break;
		}
	});

	data->websocket->start();
}

// Forward declaration: centralized settings loader (definition below update()).
static void load_settings_into_data(deepgram_caption_data *data, obs_data_t *settings);

// ─── Hotkey: Toggle Start/Stop Caption ───
static void hotkey_toggle_caption(void *private_data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *data = static_cast<deepgram_caption_data *>(private_data);

	obs_data_t *settings = obs_source_get_settings(data->source);
	load_settings_into_data(data, settings);
	obs_data_release(settings);

	if (data->captioning)
		stop_captioning(data);
	else
		start_captioning(data);
}

// ─── Callback functions ───

static const char *deepgram_caption_get_name(void *)
{
	return "Deepgram Captions";
}

static void *deepgram_caption_create(obs_data_t *settings, obs_source_t *source)
{
	auto *data = new deepgram_caption_data();
	data->source = source;

	// Read font settings
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		data->font_face = obs_data_get_string(font_obj, "face");
		data->font_style = obs_data_get_string(font_obj, "style");
		data->font_size = (int)obs_data_get_int(font_obj, "size");
		data->font_flags = (int)obs_data_get_int(font_obj, "flags");
		obs_data_release(font_obj);
	}

	obs_data_t *ts = obs_data_create();
	obs_data_set_string(ts, "text", "Deepgram Captions Ready!");
#ifdef _WIN32
	data->text_source = obs_source_create_private("text_gdiplus", "deepgram_text", ts);
#else
	data->text_source = obs_source_create_private("text_ft2_source_v2", "deepgram_text", ts);
#endif
	obs_data_release(ts);

	data->hotkey_id = obs_hotkey_register_source(source, "deepgram_toggle_caption",
						     "Toggle Deepgram Captions", hotkey_toggle_caption,
						     data);

	obs_log(LOG_INFO, "caption source created");
	return data;
}

static void deepgram_caption_destroy(void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	stop_captioning(data);
	if (data->hotkey_id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(data->hotkey_id);
	if (data->text_source)
		obs_source_release(data->text_source);
	delete data;
}

// Centralized settings -> data loader. Used by update(), button callbacks, and hotkey.
static void load_settings_into_data(deepgram_caption_data *data, obs_data_t *settings)
{
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		data->font_face = obs_data_get_string(font_obj, "face");
		data->font_style = obs_data_get_string(font_obj, "style");
		data->font_size = (int)obs_data_get_int(font_obj, "size");
		data->font_flags = (int)obs_data_get_int(font_obj, "flags");
		obs_data_release(font_obj);
	}

	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->model = obs_data_get_string(settings, "model");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->smart_format = obs_data_get_bool(settings, "smart_format");
	data->punctuate = obs_data_get_bool(settings, "punctuate");
	data->interim_results = obs_data_get_bool(settings, "interim_results");
	data->endpointing_ms = (int)obs_data_get_int(settings, "endpointing_ms");
	data->utterance_end_ms = (int)obs_data_get_int(settings, "utterance_end_ms");

	data->keyterms = obs_data_get_string(settings, "keyterms");
	data->diarize = obs_data_get_bool(settings, "diarize");
	data->redact_mode = obs_data_get_string(settings, "redact_mode");
	data->profanity_filter = obs_data_get_bool(settings, "profanity_filter");
	data->numerals = obs_data_get_bool(settings, "numerals");
	data->filler_words = obs_data_get_bool(settings, "filler_words");
	data->detect_entities = obs_data_get_bool(settings, "detect_entities");
	data->mip_opt_out = obs_data_get_bool(settings, "mip_opt_out");

	data->color1 = (uint32_t)obs_data_get_int(settings, "color1");
	data->color2 = (uint32_t)obs_data_get_int(settings, "color2");
	data->outline = obs_data_get_bool(settings, "outline");
	data->drop_shadow = obs_data_get_bool(settings, "drop_shadow");
	data->custom_width = (int)obs_data_get_int(settings, "custom_width");
	data->word_wrap = obs_data_get_bool(settings, "word_wrap");
}

static void deepgram_caption_update(void *private_data, obs_data_t *settings)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);

	load_settings_into_data(data, settings);

	if (!data->captioning && !data->connected) {
		if (!data->api_key.empty())
			update_text_display(data, "Deepgram Captions Ready!");
		else
			update_text_display(data, "[Set API Key in Properties]");
	}
}

// ─── Button callbacks ───
static bool on_test_clicked(obs_properties_t *, obs_property_t *, void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	obs_data_t *settings = obs_source_get_settings(data->source);
	load_settings_into_data(data, settings);
	obs_data_release(settings);

	if (data->connected) {
		data->stopping = true;
		data->websocket->stop();
		data->websocket.reset();
		data->connected = false;
		data->stopping = false;
		update_text_display(data, "Deepgram Captions Ready!");
	} else {
		test_connection(data);
	}
	return false;
}

static bool on_start_stop_clicked(obs_properties_t *, obs_property_t *property, void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);

	obs_data_t *settings = obs_source_get_settings(data->source);
	load_settings_into_data(data, settings);
	obs_data_release(settings);

	if (data->captioning) {
		stop_captioning(data);
		obs_property_set_description(property, "Start Caption");
	} else {
		start_captioning(data);
		obs_property_set_description(property, "Stop Caption");
	}
	return true;
}

// ─── Enumerate audio sources ───
static bool enum_audio_sources(void *param, obs_source_t *source)
{
	auto *list = static_cast<obs_property_t *>(param);
	uint32_t flags = obs_source_get_output_flags(source);
	if (flags & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(source);
		if (name && strlen(name) > 0)
			obs_property_list_add_string(list, name, name);
	}
	return true;
}

// Properties UI
static obs_properties_t *deepgram_caption_get_properties(void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	obs_properties_t *props = obs_properties_create();

	// API Key
	obs_properties_add_text(props, "api_key", "Deepgram API Key", OBS_TEXT_PASSWORD);

	// Audio source selection
	obs_property_t *audio_list =
		obs_properties_add_list(props, "audio_source", "Audio Source", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(audio_list, "(Select audio source)", "");
	obs_enum_sources(enum_audio_sources, audio_list);

	// Model selection
	obs_property_t *model_list =
		obs_properties_add_list(props, "model", "Model", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(model_list, "Nova-3 (Latest)", "nova-3");
	obs_property_list_add_string(model_list, "Nova-3 General", "nova-3-general");
	obs_property_list_add_string(model_list, "Nova-3 Medical", "nova-3-medical");
	obs_property_list_add_string(model_list, "Nova-2", "nova-2");
	obs_property_list_add_string(model_list, "Nova-2 General", "nova-2-general");
	obs_property_set_long_description(
		model_list,
		"Deepgram transcription model.\n"
		"• Nova-3 (Latest): Best overall accuracy, recommended for most use cases.\n"
		"• Nova-3 Medical: Tuned for medical terminology.\n"
		"• Nova-2: Previous generation, use only for compatibility.");

	// Language selection
	obs_property_t *lang =
		obs_properties_add_list(props, "language", "Language", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(lang, "Korean", "ko");
	obs_property_list_add_string(lang, "English (US)", "en-US");
	obs_property_list_add_string(lang, "English (UK)", "en-GB");
	obs_property_list_add_string(lang, "Japanese", "ja");
	obs_property_list_add_string(lang, "Chinese (Simplified)", "zh");
	obs_property_list_add_string(lang, "Chinese (Traditional)", "zh-TW");
	obs_property_list_add_string(lang, "Spanish", "es");
	obs_property_list_add_string(lang, "French", "fr");
	obs_property_list_add_string(lang, "German", "de");
	obs_property_list_add_string(lang, "Portuguese", "pt");
	obs_property_list_add_string(lang, "Italian", "it");
	obs_property_list_add_string(lang, "Dutch", "nl");
	obs_property_list_add_string(lang, "Russian", "ru");
	obs_property_list_add_string(lang, "Hindi", "hi");
	obs_property_list_add_string(lang, "Multilingual (Auto)", "multi");
	obs_property_set_long_description(
		lang,
		"Target language for transcription.\n"
		"Use 'Multilingual (Auto)' for mixed-language content (Nova-3 only).");

	// Transcription options
	obs_property_t *p_smart = obs_properties_add_bool(props, "smart_format", "Smart Format");
	obs_property_set_long_description(
		p_smart,
		"Applies formatting like capitalization, numerals, dates, times, and "
		"currency to improve readability. Recommended: ON.");

	obs_property_t *p_punct = obs_properties_add_bool(props, "punctuate", "Punctuation");
	obs_property_set_long_description(
		p_punct,
		"Adds punctuation marks (periods, commas, question marks) to the transcript.\n"
		"Recommended: ON.");

	obs_property_t *p_interim =
		obs_properties_add_bool(props, "interim_results", "Show Interim Results");
	obs_property_set_long_description(
		p_interim,
		"Shows real-time partial transcripts as you speak, before segments are "
		"finalized.\nRecommended: ON for live captioning (lower perceived latency).");

	// Endpointing sensitivity
	obs_property_t *p_endpoint = obs_properties_add_int_slider(
		props, "endpointing_ms", "Endpointing (ms)", 100, 1000, 50);
	obs_property_set_long_description(
		p_endpoint,
		"Silence duration (ms) before Deepgram finalizes a speech segment.\n"
		"Lower = shorter caption segments, higher = longer accumulated text.\n"
		"\n"
		"Recommended:\n"
		"• 100-200ms: Continuous speech (sermons, lectures) — prevents screen overflow\n"
		"• 300ms: General conversation (default)\n"
		"• 500-1000ms: Short Q&A, when you want fewer segment breaks");

	// Utterance end (buffer clear gap)
	obs_property_t *p_utter = obs_properties_add_int_slider(
		props, "utterance_end_ms", "Utterance End (ms)", 1000, 5000, 100);
	obs_property_set_long_description(
		p_utter,
		"Silence gap (ms) after the last finalized word that triggers an "
		"UtteranceEnd event,\nwhich clears the caption buffer. Minimum 1000ms "
		"(Deepgram spec).\n"
		"\n"
		"Recommended:\n"
		"• 1000ms: Fast buffer clear for sermons/lectures (default)\n"
		"• 1500-2000ms: General use, keeps utterance on screen longer\n"
		"• 3000-5000ms: When you want long lines to persist");

	// ─── Nova-3 Accuracy & Formatting ───

	obs_property_t *p_keyterms =
		obs_properties_add_text(props, "keyterms", "Keyterms (one per line)", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(
		p_keyterms,
		"Nova-3 Keyterm Prompting: boost recognition accuracy for proper nouns, "
		"product names, jargon, or rare phrases.\n"
		"One keyterm per line. Multi-word phrases are supported (e.g., \"Deepgram Nova\").\n"
		"\n"
		"Limits:\n"
		"• Up to 100 keyterms\n"
		"• 500 tokens total across all keyterms\n"
		"• Nova-3 only (ignored by Nova-2)\n"
		"\n"
		"Use for: speaker names, brand names, technical terms, city/place names.");

	obs_property_t *p_diarize = obs_properties_add_bool(props, "diarize", "Speaker Diarization");
	obs_property_set_long_description(
		p_diarize,
		"Identify different speakers and prefix captions with [S1], [S2], etc.\n"
		"Useful for interviews, panels, and multi-speaker recordings.\n"
		"Streaming mode returns speaker numbers only (no confidence).");

	obs_property_t *p_redact = obs_properties_add_list(props, "redact_mode", "Redact Sensitive Info",
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p_redact, "Off", "off");
	obs_property_list_add_string(p_redact, "Numbers", "numbers");
	obs_property_list_add_string(p_redact, "PCI (credit cards)", "pci");
	obs_property_list_add_string(p_redact, "SSN", "ssn");
	obs_property_list_add_string(p_redact, "All (numbers + PCI + SSN)", "all");
	obs_property_set_long_description(
		p_redact,
		"Replace sensitive information in the transcript with placeholders.\n"
		"Recommended for live broadcasts that may expose callers' account info.");

	obs_property_t *p_prof =
		obs_properties_add_bool(props, "profanity_filter", "Profanity Filter");
	obs_property_set_long_description(
		p_prof, "Mask profanity with asterisks. Useful for broadcast compliance.");

	obs_property_t *p_num = obs_properties_add_bool(props, "numerals", "Numerals");
	obs_property_set_long_description(
		p_num,
		"Render spoken numbers as digits (e.g., \"twenty three\" -> \"23\"). "
		"Independent of Smart Format.");

	obs_property_t *p_filler = obs_properties_add_bool(props, "filler_words", "Keep Filler Words");
	obs_property_set_long_description(
		p_filler,
		"Include disfluencies like \"um\", \"uh\" in the transcript. "
		"Turn OFF (default) for cleaner captions.");

	obs_property_t *p_ent =
		obs_properties_add_bool(props, "detect_entities", "Detect Entities");
	obs_property_set_long_description(
		p_ent, "Identify named entities (people, places, organizations) in the transcript.");

	obs_property_t *p_mip =
		obs_properties_add_bool(props, "mip_opt_out", "Opt out of Model Improvement");
	obs_property_set_long_description(
		p_mip,
		"Prevent Deepgram from using your audio for model training. "
		"Enable for sensitive or private content.");

	// ─── Text Style ───

	// Font selection (system font dialog)
	obs_properties_add_font(props, "font", "Font");

	// Text color
	obs_properties_add_color(props, "color1", "Text Color");
	obs_properties_add_color(props, "color2", "Text Color 2 (Gradient)");

	// Text effects
	obs_properties_add_bool(props, "outline", "Outline");
	obs_properties_add_bool(props, "drop_shadow", "Drop Shadow");

	// Text layout
	obs_properties_add_int(props, "custom_width", "Custom Text Width (0=auto)", 0, 4096, 1);
	obs_properties_add_bool(props, "word_wrap", "Word Wrap");

	// Buttons
	obs_properties_add_button(props, "test_connection", "Test Connection", on_test_clicked);

	const char *btn_text = (data && data->captioning) ? "Stop Caption" : "Start Caption";
	obs_properties_add_button(props, "start_stop", btn_text, on_start_stop_clicked);

	return props;
}

static void deepgram_caption_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "api_key", "");
	obs_data_set_default_string(settings, "language", "ko");
	obs_data_set_default_string(settings, "model", "nova-3");
	obs_data_set_default_string(settings, "audio_source", "");
	obs_data_set_default_bool(settings, "smart_format", true);
	obs_data_set_default_bool(settings, "punctuate", true);
	obs_data_set_default_bool(settings, "interim_results", true);
	obs_data_set_default_int(settings, "endpointing_ms", 300);
	obs_data_set_default_int(settings, "utterance_end_ms", 1000);

	// Nova-3 feature defaults
	obs_data_set_default_string(settings, "keyterms", "");
	obs_data_set_default_bool(settings, "diarize", false);
	obs_data_set_default_string(settings, "redact_mode", "off");
	obs_data_set_default_bool(settings, "profanity_filter", false);
	obs_data_set_default_bool(settings, "numerals", false);
	obs_data_set_default_bool(settings, "filler_words", false);
	obs_data_set_default_bool(settings, "detect_entities", false);
	obs_data_set_default_bool(settings, "mip_opt_out", false);

	// Font defaults (obs_data_t object)
	obs_data_t *font_obj = obs_data_create();
#ifdef _WIN32
	obs_data_set_default_string(font_obj, "face", "Malgun Gothic");
#else
	obs_data_set_default_string(font_obj, "face", "Apple SD Gothic Neo");
#endif
	obs_data_set_default_string(font_obj, "style", "Regular");
	obs_data_set_default_int(font_obj, "size", 48);
	obs_data_set_default_int(font_obj, "flags", 0);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);

	// Text style defaults
	obs_data_set_default_int(settings, "color1", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "color2", 0xFFFFFFFF);
	obs_data_set_default_bool(settings, "outline", false);
	obs_data_set_default_bool(settings, "drop_shadow", false);
	obs_data_set_default_int(settings, "custom_width", 0);
	obs_data_set_default_bool(settings, "word_wrap", false);
}

static uint32_t deepgram_caption_get_width(void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	return data->text_source ? obs_source_get_width(data->text_source) : 0;
}

static uint32_t deepgram_caption_get_height(void *private_data)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	return data->text_source ? obs_source_get_height(data->text_source) : 0;
}

static void deepgram_caption_video_render(void *private_data, gs_effect_t *)
{
	auto *data = static_cast<deepgram_caption_data *>(private_data);
	if (data->text_source)
		obs_source_video_render(data->text_source);
}

// ─── Source registration ───
static obs_source_info deepgram_caption_source_info = {};

bool obs_module_load(void)
{
	deepgram_caption_source_info.id = "deepgram_caption";
	deepgram_caption_source_info.type = OBS_SOURCE_TYPE_INPUT;
	deepgram_caption_source_info.output_flags = OBS_SOURCE_VIDEO;
	deepgram_caption_source_info.get_name = deepgram_caption_get_name;
	deepgram_caption_source_info.create = deepgram_caption_create;
	deepgram_caption_source_info.destroy = deepgram_caption_destroy;
	deepgram_caption_source_info.update = deepgram_caption_update;
	deepgram_caption_source_info.get_properties = deepgram_caption_get_properties;
	deepgram_caption_source_info.get_defaults = deepgram_caption_get_defaults;
	deepgram_caption_source_info.get_width = deepgram_caption_get_width;
	deepgram_caption_source_info.get_height = deepgram_caption_get_height;
	deepgram_caption_source_info.video_render = deepgram_caption_video_render;

	obs_register_source(&deepgram_caption_source_info);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
