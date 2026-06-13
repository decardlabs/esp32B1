/**
 * Voice TTS - Frontend Logic
 * Handles server communication, UI updates, and user interactions.
 */

// ============================================================
// State
// ============================================================

const state = {
    serverOk: false,
    iflytekAvailable: false,
    volcanoAvailable: false,
    ffmpegAvailable: false,
    isGenerating: false,
    formatsCache: {},
    voicesCache: {},
};

// ============================================================
// DOM references
// ============================================================

const $ = (sel) => document.querySelector(sel);
const $$ = (sel) => document.querySelectorAll(sel);

const dom = {
    textInput:     $('#textInput'),
    charCount:     $('#charCount'),
    engineSelect:  $('#engineSelect'),
    voiceSelect:   $('#voiceSelect'),
    formatSelect:  $('#formatSelect'),
    generateBtn:   $('#generateBtn'),
    loadingOverlay:$('#loadingOverlay'),
    resultSection: $('#resultSection'),
    audioPlayer:   $('#audioPlayer'),
    resultInfo:    $('#resultInfo'),
    downloadLink:  $('#downloadLink'),
    copyResultBtn: $('#copyResultBtn'),
    logsArea:      $('#logsArea'),
    clearLogsBtn:  $('#clearLogsBtn'),
    statusDot:     $('#statusDot'),
    statusText:    $('#statusText'),
};

// ============================================================
// Utilities
// ============================================================

function showToast(msg) {
    let el = document.querySelector('.toast');
    if (!el) {
        el = document.createElement('div');
        el.className = 'toast';
        document.body.appendChild(el);
    }
    el.textContent = msg;
    el.classList.add('show');
    clearTimeout(el._timer);
    el._timer = setTimeout(() => el.classList.remove('show'), 2500);
}

function formatSize(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / 1048576).toFixed(1) + ' MB';
}

function formatTime(iso) {
    const d = new Date(iso);
    return d.toLocaleTimeString('zh-CN', { hour12: false });
}

function escapeHtml(str) {
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
}

// ============================================================
// Server communication
// ============================================================

async function apiGet(url) {
    const resp = await fetch(url);
    if (!resp.ok) {
        const detail = await resp.json().catch(() => ({}));
        throw new Error(detail.detail || `HTTP ${resp.status}`);
    }
    return resp.json();
}

async function apiPost(url, body) {
    const resp = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
    });
    if (!resp.ok) {
        const detail = await resp.json().catch(() => ({}));
        throw new Error(detail.detail || `HTTP ${resp.status}`);
    }
    return resp.json();
}

// ============================================================
// Initialization – fetch server status
// ============================================================

async function init() {
    try {
        const data = await apiGet('/api/status');
        state.serverOk = true;
        state.iflytekAvailable = data.engines.iflytek.available;
        state.volcanoAvailable = data.engines.volcano.available;
        state.ffmpegAvailable = data.ffmpeg_available;
        state.formatsCache = data.formats_all;

        dom.statusDot.className = 'status-dot ok';
        dom.statusText.textContent = '服务器已连接';

        // populate initial engine's data
        const engine = dom.engineSelect.value;
        await Promise.all([
            loadVoices(engine),
            loadFormats(engine),
        ]);

        enableForm(true);
        dom.generateBtn.disabled = false;

        // fetch logs
        loadLogs();

    } catch (err) {
        state.serverOk = false;
        dom.statusDot.className = 'status-dot error';
        dom.statusText.textContent = '服务器未连接: ' + err.message;
        dom.generateBtn.disabled = true;
    }
}

function enableForm(on) {
    dom.engineSelect.disabled = !on;
    dom.voiceSelect.disabled = !on;
    dom.formatSelect.disabled = !on;
}

// ============================================================
// Load voices for selected engine
// ============================================================

async function loadVoices(engine) {
    dom.voiceSelect.innerHTML = '<option value="">加载音色中...</option>';
    dom.voiceSelect.disabled = true;

    try {
        const data = await apiGet(`/api/voices?engine=${engine}`);
        state.voicesCache[engine] = data.voices;
        populateVoices(data.voices);
    } catch (err) {
        dom.voiceSelect.innerHTML = `<option value="">音色加载失败: ${err.message}</option>`;
    } finally {
        dom.voiceSelect.disabled = false;
    }
}

function populateVoices(voices) {
    dom.voiceSelect.innerHTML = '';
    voices.forEach((v) => {
        const opt = document.createElement('option');
        opt.value = v.id;
        const genderLabel = v.gender === 'female' ? '女' : '男';
        opt.textContent = `${v.name} (${genderLabel}) — ${v.description}`;
        dom.voiceSelect.appendChild(opt);
    });
    // select first
    if (voices.length > 0) dom.voiceSelect.selectedIndex = 0;
}

// ============================================================
// Load formats for selected engine
// ============================================================

function loadFormats(engine) {
    const allFormats = state.formatsCache[engine] || [];
    // filter out ffmpeg-required formats if ffmpeg not available
    let available = allFormats;
    if (!state.ffmpegAvailable) {
        available = allFormats.filter((f) => !f.requires_ffmpeg);
    }
    populateFormats(available);
}

function populateFormats(formats) {
    dom.formatSelect.innerHTML = '';
    formats.forEach((f) => {
        const opt = document.createElement('option');
        opt.value = f.id;
        opt.textContent = `${f.label} — ${f.description}`;
        dom.formatSelect.appendChild(opt);
    });
    // prefer wav if available
    const wavIdx = formats.findIndex((f) => f.id === 'wav');
    if (wavIdx >= 0) dom.formatSelect.selectedIndex = wavIdx;
}

// ============================================================
// Engine change → reload voices & formats
// ============================================================

dom.engineSelect.addEventListener('change', () => {
    const engine = dom.engineSelect.value;
    loadVoices(engine);
    loadFormats(engine);
});

// ============================================================
// Text input char count
// ============================================================

dom.textInput.addEventListener('input', () => {
    const len = dom.textInput.value.length;
    dom.charCount.textContent = len;
    dom.charCount.className = '';
    if (len > 4500) dom.charCount.classList.add('warning');
    if (len > 5000) dom.charCount.classList.add('over');
});

// ============================================================
// Generate button
// ============================================================

dom.generateBtn.addEventListener('click', async () => {
    if (state.isGenerating) return;

    const text = dom.textInput.value.trim();
    if (!text) {
        showToast('请输入要合成语音的文本');
        dom.textInput.focus();
        return;
    }
    if (text.length > 5000) {
        showToast('文本过长，请限制在 5000 字符以内');
        return;
    }

    const engine = dom.engineSelect.value;

    // Check engine availability
    if (engine === 'iflytek' && !state.iflytekAvailable) {
        showToast('讯飞 TTS 未配置，请在 config.py 中填写 API 凭证');
        return;
    }
    if (engine === 'volcano' && !state.volcanoAvailable) {
        showToast('火山引擎 TTS 未配置，请在 config.py 中填写 API 凭证');
        return;
    }

    const voice = dom.voiceSelect.value;
    const format = dom.formatSelect.value;

    state.isGenerating = true;
    dom.generateBtn.disabled = true;
    dom.generateBtn.querySelector('.btn-text').textContent = '生成中...';
    dom.loadingOverlay.style.display = 'flex';

    try {
        const result = await apiPost('/api/tts', {
            text,
            engine,
            format,
            voice: voice || undefined,
        });

        if (result.success) {
            showResult(result);
            showToast('合成成功');
            loadLogs();
        } else {
            showToast('合成失败: ' + result.message);
        }

    } catch (err) {
        showToast('请求失败: ' + err.message);
        loadLogs(); // logs may have been written server-side
    } finally {
        state.isGenerating = false;
        dom.generateBtn.disabled = false;
        dom.generateBtn.querySelector('.btn-text').textContent = '生成语音';
        dom.loadingOverlay.style.display = 'none';
    }
});

// ============================================================
// Display result
// ============================================================

function showResult(result) {
    dom.resultSection.style.display = 'block';

    // Audio player — add cache buster to force reload
    const cacheBuster = `?t=${Date.now()}`;
    dom.audioPlayer.src = result.file_url + cacheBuster;
    dom.audioPlayer.load();
    dom.audioPlayer.play().catch(() => {}); // autoplay may be blocked

    // Info
    dom.resultInfo.innerHTML = `
        <span>📄 ${result.filename}</span>
        <span>📦 ${formatSize(result.file_size)}</span>
        <span>⏱ ${result.duration_ms.toFixed(0)} ms</span>
    `;

    // Download link
    dom.downloadLink.href = result.file_url;
    dom.downloadLink.download = result.filename;

    // Copy filename
    dom.copyResultBtn.onclick = () => {
        navigator.clipboard.writeText(result.filename).then(() => {
            showToast('文件名已复制');
        }).catch(() => {
            showToast('复制失败，请手动复制');
        });
    };
}

// ============================================================
// Logs
// ============================================================

async function loadLogs() {
    try {
        const data = await apiGet('/api/logs?limit=50');
        renderLogs(data.logs);
    } catch (err) {
        // silent
    }
}

function renderLogs(logs) {
    if (logs.length === 0) {
        dom.logsArea.innerHTML = '<div class="logs-placeholder">暂无日志</div>';
        return;
    }

    dom.logsArea.innerHTML = logs.map((entry) => {
        const statusClass = entry.status === 'success' ? 'success' : 'error';
        const statusIcon = entry.status === 'success' ? '✓' : '✗';
        const elapsed = entry.elapsed_ms ? `${entry.elapsed_ms}ms` : '';
        const errorLine = entry.error
            ? `<div class="log-error">${escapeHtml(entry.error)}</div>`
            : '';

        return `
            <div class="log-entry">
                <span class="log-time">${formatTime(entry.time)}</span>
                <span class="log-status ${statusClass}">${statusIcon}</span>
                <span class="log-engine">${entry.engine}</span>
                <span class="log-format">${entry.format}</span>
                <span class="log-elapsed">${elapsed}</span>
                <div class="log-text">${escapeHtml(entry.text_preview || '')}</div>
                ${errorLine}
            </div>
        `;
    }).join('');
}

dom.clearLogsBtn.addEventListener('click', () => {
    dom.logsArea.innerHTML = '<div class="logs-placeholder">暂无日志</div>';
});

// ============================================================
// Poll logs periodically
// ============================================================

setInterval(() => {
    if (state.serverOk) loadLogs();
}, 5000);

// ============================================================
// Keyboard shortcut: Ctrl+Enter to generate
// ============================================================

dom.textInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        dom.generateBtn.click();
    }
});

// ============================================================
// Boot
// ============================================================

init();
