/* Watch Server dashboard: no build step, no external frontend dependencies. */
(() => {
  "use strict";

  const $ = (selector) => document.querySelector(selector);
  const $$ = (selector) => Array.from(document.querySelectorAll(selector));
  const state = {
    token: localStorage.getItem("watch_admin_token") || "",
    status: null,
    providers: null,
    files: [],
    messages: [],
    busy: false,
    tab: "chat",
    logs: [],
    logCursor: 0,
    logTimer: null,
    logBusy: false,
    logLevel: "",
  };

  const TITLES = {
    chat: "AI 对话",
    tts: "TTS 语音",
    files: "文件上传",
    video: "视频播放",
    logs: "运行日志",
    status: "运行状态",
  };

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = text;
    return node;
  }

  function toast(message, isError = false) {
    const box = $("#toast");
    box.textContent = message;
    box.classList.toggle("err", isError);
    box.classList.add("show");
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => box.classList.remove("show"), 3200);
  }

  function showTokenModal() {
    $("#tokenModal").hidden = false;
    $("#tokenInput").value = state.token;
    $("#tokenInput").focus();
  }

  function authHeaders(extra = {}) {
    return state.token ? { ...extra, Authorization: "Bearer " + state.token } : extra;
  }

  function withToken(url) {
    if (!state.token) return url;
    const separator = url.includes("?") ? "&" : "?";
    return url + separator + "token=" + encodeURIComponent(state.token);
  }

  async function api(path, options = {}) {
    const response = await fetch(path, {
      ...options,
      headers: authHeaders(options.headers || {}),
    });
    if (response.status === 401) {
      showTokenModal();
      throw new Error("需要 ADMIN_TOKEN");
    }
    if (!response.ok) {
      let detail = "HTTP " + response.status;
      try {
        const body = await response.json();
        detail = body.error || body.detail || detail;
      } catch (_) { /* not JSON */ }
      throw new Error(detail);
    }
    return response;
  }

  async function apiJson(path, options = {}) {
    return (await api(path, options)).json();
  }

  function formatBytes(value) {
    const size = Number(value) || 0;
    if (size < 1024) return size + " B";
    if (size < 1024 * 1024) return (size / 1024).toFixed(1) + " KB";
    if (size < 1024 * 1024 * 1024) return (size / 1024 / 1024).toFixed(1) + " MB";
    return (size / 1024 / 1024 / 1024).toFixed(2) + " GB";
  }

  function formatDate(epoch) {
    if (!epoch) return "";
    return new Date(Number(epoch) * 1000).toLocaleString();
  }

  function setTab(tab) {
    state.tab = tab;
    $$(".nav-item").forEach((item) => item.classList.toggle("active", item.dataset.tab === tab));
    $$(".panel").forEach((panel) => panel.classList.toggle("active", panel.id === "panel-" + tab));
    $("#pageTitle").textContent = TITLES[tab] || tab;
    if (tab === "files" || tab === "video") loadFiles().catch((error) => toast(error.message, true));
    if (tab === "logs") startLogs();
    else stopLogs();
  }

  async function refreshStatus() {
    try {
      const response = await fetch("/api/status");
      const status = await response.json();
      state.status = status;
      $("#serverPill").textContent = "online";
      $("#serverPill").className = "pill ok";
      $("#sidebarDot").className = "dot ok";
      $("#sidebarState").textContent = "server online";
      renderStatus(status);
      await loadProviders();
      await loadVoices();
    } catch (error) {
      $("#serverPill").textContent = "offline";
      $("#serverPill").className = "pill err";
      $("#sidebarDot").className = "dot err";
      $("#sidebarState").textContent = "unreachable";
      toast(error.message, true);
    }
  }

  async function loadProviders() {
    try {
      state.providers = await apiJson("/api/ai/providers");
      const info = (state.providers.providers || {}).deepseek || {};
      $("#chatHint").textContent = info.available
        ? "DeepSeek 就绪 · " + (info.model || "")
        : "DEEPSEEK_API_KEY 未配置";
      updateModelPlaceholder();
    } catch (error) {
      if (error.message !== "需要 ADMIN_TOKEN") toast(error.message, true);
    }
  }

  function updateModelPlaceholder() {
    const info = (state.providers?.providers || {}).deepseek || {};
    $("#modelInput").placeholder = info.model || "deepseek-chat";
  }

  function renderStatus(status) {
    const grid = $("#statusGrid");
    grid.innerHTML = "";
    const config = status.config || {};
    const ai = status.ai?.providers?.deepseek || {};
    const tts = status.tts || {};
    const asr = status.asr || {};
    const media = status.media || {};
    const cards = [
      ["AI", ai.available ? "ONLINE" : "OFFLINE", (ai.model || "") + " · " + (ai.detail || "")],
      ["ASR", asr.available ? "READY" : "OFF", (asr.provider || "") + " · " + (asr.model || "")],
      ["TTS", tts.available ? "READY" : "OFF", (tts.provider || "") + " · " + (tts.voice || "")],
      ["媒体", String(media.total || 0), "video " + (media.video || 0) + " / image " + (media.image || 0) + " / audio " + (media.audio || 0)],
      ["鉴权", config.auth_required ? "TOKEN" : "OPEN", config.watch_auth_required ? "watch token enabled" : "watch token off"],
    ];
    cards.forEach(([title, value, detail]) => {
      const card = el("div", "status-card");
      card.appendChild(el("h3", "", title));
      card.appendChild(el("strong", "", value));
      card.appendChild(el("p", "", detail));
      grid.appendChild(card);
    });
    $("#statusJson").textContent = JSON.stringify(status, null, 2);
    const ttsPill = $("#ttsPill");
    ttsPill.textContent = tts.available ? "ready" : "off";
    ttsPill.className = "pill " + (tts.available ? "ok" : "err");
    $("#ttsDetail").textContent = tts.detail || "";
  }

  function logClass(level) {
    const value = String(level || "").toLowerCase();
    return value === "warning" || value === "error" || value === "critical" ? value : "info";
  }

  function formatLogTime(value) {
    const date = new Date(value);
    return Number.isNaN(date.getTime()) ? String(value || "") : date.toLocaleTimeString([], { hour12: false });
  }

  function renderLogs() {
    const view = $("#logView");
    if (!view) return;
    const level = state.logLevel;
    const items = state.logs.filter((item) => !level || String(item.level || "").toUpperCase() === level);
    if (!items.length) {
      view.innerHTML = "";
      view.appendChild(el("div", "log-empty", level ? "该级别暂无日志" : "暂无日志；连接手表或操作上方功能后会出现记录。"));
      return;
    }
    const follow = $("#logFollow").checked;
    const fragment = document.createDocumentFragment();
    items.forEach((item) => {
      const row = el("div", "log-row " + logClass(item.level));
      row.appendChild(el("span", "log-time", formatLogTime(item.ts)));
      row.appendChild(el("span", "log-level", item.level || "INFO"));
      row.appendChild(el("span", "log-logger", item.logger || "server"));
      row.appendChild(el("span", "log-msg", item.message || ""));
      fragment.appendChild(row);
    });
    view.replaceChildren(fragment);
    if (follow) view.scrollTop = view.scrollHeight;
  }

  function stopLogs() {
    if (state.logTimer) {
      clearInterval(state.logTimer);
      state.logTimer = null;
    }
    state.logBusy = false;
  }

  async function refreshLogs() {
    if (state.tab !== "logs" || state.logBusy) return;
    state.logBusy = true;
    try {
      const data = await apiJson("/api/logs?after=" + state.logCursor + "&limit=200");
      const cursor = Number(data.cursor) || 0;
      if (cursor < state.logCursor) {
        state.logs = [];
        state.logCursor = 0;
      }
      const items = Array.isArray(data.items) ? data.items : [];
      if (items.length) {
        state.logs = state.logs.concat(items).slice(-500);
      }
      state.logCursor = Math.max(state.logCursor, cursor);
      renderLogs();
    } catch (error) {
      stopLogs();
      if (error.message !== "需要 ADMIN_TOKEN") toast(error.message, true);
    } finally {
      state.logBusy = false;
    }
  }

  function startLogs() {
    stopLogs();
    refreshLogs();
    state.logTimer = setInterval(refreshLogs, 1000);
  }

  function scrollChat() {
    const box = $("#chatMessages");
    box.scrollTop = box.scrollHeight;
  }

  function addMessage(role, text) {
    const empty = $("#chatEmpty");
    if (empty) empty.remove();
    const row = el("div", "message " + role);
    row.appendChild(el("div", "avatar", role === "user" ? "YOU" : "AI"));
    const bubble = el("div", "bubble");
    bubble.appendChild(el("span", "role", role === "user" ? "USER" : "ASSISTANT"));
    const content = el("div", "message-text", text);
    bubble.appendChild(content);
    row.appendChild(bubble);
    $("#chatMessages").appendChild(row);
    scrollChat();
    return content;
  }

  function renderChat() {
    const box = $("#chatMessages");
    box.innerHTML = "";
    if (!state.messages.length) {
      const empty = el("div", "empty-state");
      empty.id = "chatEmpty";
      empty.appendChild(el("div", "empty-icon", "◈"));
      empty.appendChild(el("h3", "", "向服务器提问"));
      empty.appendChild(el("p", "", "支持普通聊天、代码问题和中文语音播报。手表与网页共用同一个 AI 后端。"));
      box.appendChild(empty);
      return;
    }
    state.messages.forEach((message) => addMessage(message.role, message.content));
  }


  async function sendChat(event) {
    event.preventDefault();
    if (state.busy) return;
    const input = $("#chatInput");
    const text = input.value.trim();
    if (!text) return;
    state.busy = true;
    $("#chatForm button").disabled = true;
    $("#chatHint").textContent = "生成中…";
    state.messages.push({ role: "user", content: text });
    addMessage("user", text);
    input.value = "";
    state.messages.push({ role: "assistant", content: "" });
    const bubble = addMessage("assistant", "").parentElement;
    bubble.classList.add("typing");
    let assistantText = "";
    try {
      const response = await api("/api/chat", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          messages: state.messages.filter((message) => message.content),
          provider: "deepseek",
          model: $("#modelInput").value.trim() || null,
          stream: true,
        }),
      });
      if (!response.body) throw new Error("浏览器不支持流式响应");
      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      let buffer = "";
      let finished = false;
      while (!finished) {
        const chunk = await reader.read();
        if (chunk.done) break;
        buffer += decoder.decode(chunk.value, { stream: true });
        let boundary;
        while ((boundary = buffer.indexOf("\n\n")) >= 0) {
          const block = buffer.slice(0, boundary);
          buffer = buffer.slice(boundary + 2);
          for (const line of block.split("\n")) {
            if (!line.startsWith("data:")) continue;
            let eventData;
            try { eventData = JSON.parse(line.slice(5).trim()); } catch (_) { continue; }
            if (eventData.type === "delta") {
              assistantText += eventData.text || "";
              bubble.querySelector(".message-text").textContent = assistantText;
              scrollChat();
            } else if (eventData.type === "error") {
              throw new Error(eventData.message || "AI 请求失败");
            } else if (eventData.type === "done") {
              finished = true;
            }
          }
        }
      }
      state.messages[state.messages.length - 1].content = assistantText || "(空回复)";
      if (!assistantText) bubble.querySelector(".message-text").textContent = "(空回复)";
    } catch (error) {
      state.messages[state.messages.length - 1].content = "错误：" + error.message;
      bubble.querySelector(".message-text").textContent = "错误：" + error.message;
      bubble.classList.add("error");
      toast(error.message, true);
    } finally {
      bubble.classList.remove("typing");
      state.busy = false;
      $("#chatForm button").disabled = false;
      $("#chatHint").textContent = "就绪";
      scrollChat();
    }
  }

  async function loadVoices() {
    try {
      const data = await apiJson("/api/tts/voices");
      const select = $("#ttsVoice");
      select.innerHTML = "";
      const voices = data.voices || [];
      if (!voices.length) {
        const option = el("option", "", "未发现 Piper 声音");
        option.value = "";
        select.appendChild(option);
      } else {
        voices.forEach((voice) => {
          const option = el("option", "", voice.name || voice.id);
          option.value = voice.id;
          select.appendChild(option);
        });
      }
      if (state.status?.tts?.available === false) {
        $("#ttsHint").textContent = state.status.tts.detail || "TTS 未配置";
      }
    } catch (error) {
      if (error.message !== "需要 ADMIN_TOKEN") toast(error.message, true);
    }
  }

  async function speak() {
    const text = $("#ttsText").value.trim();
    if (!text) { toast("请输入文本", true); return; }
    const button = $("#ttsSpeak");
    button.disabled = true;
    $("#ttsHint").textContent = "合成中…";
    try {
      const response = await api("/api/tts", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ text, voice: $("#ttsVoice").value || null }),
      });
      const blob = await response.blob();
      const url = URL.createObjectURL(blob);
      const audio = $("#ttsAudio");
      if (audio.dataset.url) URL.revokeObjectURL(audio.dataset.url);
      audio.src = url;
      audio.dataset.url = url;
      audio.play().catch(() => {});
      const download = $("#ttsDownload");
      download.href = url;
      download.hidden = false;
      $("#ttsHint").textContent = "完成 · " + formatBytes(blob.size);
      toast("语音生成完成");
    } catch (error) {
      $("#ttsHint").textContent = error.message;
      toast(error.message, true);
    } finally {
      button.disabled = false;
    }
  }


  function contentUrl(file) {
    return withToken(file.content_url || ("/api/files/" + file.id + "/content"));
  }

  async function loadFiles() {
    const data = await apiJson("/api/files?limit=500");
    state.files = data.items || [];
    renderFiles();
    renderVideos();
    if (state.status) state.status.media = data.stats;
  }

  function renderFiles() {
    const container = $("#fileList");
    const filter = $("#fileFilter").value;
    const files = state.files.filter((file) => !filter || file.kind === filter);
    container.innerHTML = "";
    if (!files.length) {
      const empty = el("div", "empty-state");
      empty.style.gridColumn = "1 / -1";
      empty.appendChild(el("div", "empty-icon", "▤"));
      empty.appendChild(el("h3", "", "还没有文件"));
      empty.appendChild(el("p", "", "拖拽上传一个 MP4、图片或音频文件。"));
      container.appendChild(empty);
      return;
    }
    files.forEach((file) => {
      const card = el("article", "file-card");
      const thumb = el("div", "file-thumb");
      if (file.kind === "image") {
        const image = el("img");
        image.loading = "lazy";
        image.alt = file.title || "image";
        image.src = contentUrl(file);
        thumb.appendChild(image);
      } else if (file.kind === "video") {
        thumb.appendChild(el("span", "", "▶"));
      } else if (file.kind === "audio") {
        thumb.appendChild(el("span", "", "◉"));
      } else {
        thumb.appendChild(el("span", "", "▤"));
      }
      card.appendChild(thumb);
      const body = el("div", "file-body");
      body.appendChild(el("div", "file-title", file.title || file.original_name || file.id));
      body.appendChild(el("div", "file-meta", file.kind + " · " + formatBytes(file.size) + " · " + formatDate(file.created_at)));
      const actions = el("div", "file-actions");
      const open = el("a", "btn ghost", file.kind === "video" ? "打开" : "预览");
      open.href = contentUrl(file);
      open.target = "_blank";
      open.rel = "noopener";
      actions.appendChild(open);
      const remove = el("button", "btn ghost danger", "删除");
      remove.type = "button";
      remove.addEventListener("click", () => deleteFile(file));
      actions.appendChild(remove);
      body.appendChild(actions);
      card.appendChild(body);
      container.appendChild(card);
    });
  }

  function renderVideos() {
    const container = $("#videoList");
    const videos = state.files.filter((file) => file.kind === "video");
    container.innerHTML = "";
    $("#videoCount").textContent = videos.length + " videos";
    if (!videos.length) {
      container.appendChild(el("div", "hint", "上传 MP4/WebM 后可在这里播放。"));
      return;
    }
    videos.forEach((video) => {
      const item = el("button", "video-item");
      item.type = "button";
      item.appendChild(el("span", "thumb", "▶"));
      const info = el("span", "info");
      info.appendChild(el("b", "", video.title || video.original_name || video.id));
      const duration = video.duration_ms ? (video.duration_ms / 1000).toFixed(1) + "s · " : "";
      info.appendChild(el("small", "", duration + formatBytes(video.size)));
      item.appendChild(info);
      item.addEventListener("click", () => playVideo(video, item));
      container.appendChild(item);
    });
  }

  function playVideo(file, item) {
    const player = $("#videoPlayer");
    player.src = contentUrl(file);
    player.load();
    $("#videoCaption").textContent = file.title || file.original_name || file.id;
    $$(".video-item").forEach((node) => node.classList.remove("active"));
    if (item) item.classList.add("active");
    player.play().catch(() => {});
  }

  async function deleteFile(file) {
    if (!confirm("删除 " + (file.title || file.original_name) + " ?")) return;
    try {
      await api("/api/files/" + encodeURIComponent(file.id), { method: "DELETE" });
      toast("已删除");
      await loadFiles();
    } catch (error) {
      toast(error.message, true);
    }
  }

  function uploadOne(file) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open("POST", "/api/files");
      if (state.token) xhr.setRequestHeader("Authorization", "Bearer " + state.token);
      xhr.upload.addEventListener("progress", (event) => {
        if (event.lengthComputable) {
          const percent = Math.round((event.loaded / event.total) * 100);
          $("#uploadBar").style.width = percent + "%";
          $("#uploadProgress").hidden = false;
        }
      });
      xhr.addEventListener("load", () => {
        if (xhr.status === 401) { showTokenModal(); reject(new Error("需要 ADMIN_TOKEN")); return; }
        if (xhr.status < 200 || xhr.status >= 300) {
          let message = "HTTP " + xhr.status;
          try { message = JSON.parse(xhr.responseText).error || message; } catch (_) {}
          reject(new Error(message));
          return;
        }
        resolve(JSON.parse(xhr.responseText));
      });
      xhr.addEventListener("error", () => reject(new Error("上传失败")));
      const form = new FormData();
      form.append("file", file);
      xhr.send(form);
    });
  }

  async function uploadFiles(fileList) {
    const files = Array.from(fileList || []);
    if (!files.length) return;
    for (const file of files) {
      toast("上传 " + file.name + " …");
      try {
        await uploadOne(file);
      } catch (error) {
        toast(file.name + ": " + error.message, true);
      }
    }
    $("#uploadProgress").hidden = true;
    $("#uploadBar").style.width = "0";
    await loadFiles();
    toast("上传完成");
  }


  function bind() {
    $$(".nav-item").forEach((item) => item.addEventListener("click", () => setTab(item.dataset.tab)));
    $("#refreshBtn").addEventListener("click", () => refreshStatus());
    $("#refreshStatus").addEventListener("click", () => refreshStatus());
    $("#logRefresh").addEventListener("click", () => refreshLogs());
    $("#logClear").addEventListener("click", () => {
      state.logs = [];
      renderLogs();
    });
    $("#logLevel").addEventListener("change", (event) => {
      state.logLevel = event.target.value;
      renderLogs();
    });
    $("#logView").addEventListener("scroll", () => {
      const view = $("#logView");
      $("#logFollow").checked = view.scrollHeight - view.scrollTop - view.clientHeight < 24;
    });
    $("#refreshFiles").addEventListener("click", () => loadFiles().catch((error) => toast(error.message, true)));
    $("#fileFilter").addEventListener("change", renderFiles);
    $("#newChatBtn").addEventListener("click", () => {
      state.messages = [];
      renderChat();
      $("#chatHint").textContent = "就绪";
    });
    $("#chatForm").addEventListener("submit", sendChat);
    $("#chatInput").addEventListener("keydown", (event) => {
      if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        $("#chatForm").requestSubmit();
      }
    });
    $("#ttsSpeak").addEventListener("click", speak);
    const dropzone = $("#dropzone");
    const input = $("#fileInput");
    dropzone.addEventListener("click", () => input.click());
    dropzone.addEventListener("keydown", (event) => {
      if (event.key === "Enter" || event.key === " ") { event.preventDefault(); input.click(); }
    });
    input.addEventListener("change", () => uploadFiles(input.files));
    ["dragenter", "dragover"].forEach((name) => dropzone.addEventListener(name, (event) => {
      event.preventDefault();
      dropzone.classList.add("drag");
    }));
    ["dragleave", "drop"].forEach((name) => dropzone.addEventListener(name, (event) => {
      event.preventDefault();
      dropzone.classList.remove("drag");
    }));
    dropzone.addEventListener("drop", (event) => uploadFiles(event.dataTransfer.files));
    $("#saveToken").addEventListener("click", () => {
      state.token = $("#tokenInput").value.trim();
      if (state.token) localStorage.setItem("watch_admin_token", state.token);
      else localStorage.removeItem("watch_admin_token");
      $("#tokenModal").hidden = true;
      boot();
    });
    $("#tokenInput").addEventListener("keydown", (event) => {
      if (event.key === "Enter") $("#saveToken").click();
    });
  }

  async function boot() {
    await refreshStatus();
    if (!state.status?.config?.auth_required || state.token) {
      try { await loadFiles(); } catch (error) { if (error.message !== "需要 ADMIN_TOKEN") toast(error.message, true); }
    }
  }

  bind();
  setTab("chat");
  boot();
})();

