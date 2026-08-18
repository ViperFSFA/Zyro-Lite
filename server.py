"""
Insignia Cowork — Flask Backend
Handles chat streaming, file ops, computer control.
"""

import os
import sys
import json
import re
import threading
import time
import queue
import requests
from functools import wraps

from flask import Flask, request, jsonify, Response, render_template, redirect, stream_with_context

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from core.config import load_config, save_config
from core.ollama_client import stream_chat, simple_chat, list_models, check_connection
from core import api_chat
from core import computer_control as cc
from core import auth_client
from core import memory

app = Flask(__name__, template_folder="templates", static_folder="static")
app.secret_key = "insignia-cowork-local"
# Hard cap on any single request body. Attachments are already capped
# client-side (see MAX_ATTACH_BYTES in index.html), but that's just UI —
# without a matching server-side limit, a modified/older client or a stray
# huge upload could still hand Flask a request body large enough to exhaust
# memory and crash the process (which is what made a big file upload, e.g.
# a large .rar, take the whole app down and force a reload). A handful of
# attachments at 25MB each plus JSON overhead comfortably fits under this.
app.config["MAX_CONTENT_LENGTH"] = 120 * 1024 * 1024  # 120 MB


@app.errorhandler(413)
def _too_large(e):
    return jsonify({"error": "Upload too large. Please attach smaller files."}), 413

# Copy logos from the top-level media/ folder into static/media/ so they're
# served by Flask. Done once at startup so templates can reference them.
import shutil as _shutil
_media_src_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "media")
_media_dst_dir = os.path.join(os.path.dirname(__file__), "static", "media")
os.makedirs(_media_dst_dir, exist_ok=True)
for _logo_name in ("logo_insignia.png", "logo_viperfsfa.png", "logo.png"):
    _src = os.path.join(_media_src_dir, _logo_name)
    _dst = os.path.join(_media_dst_dir, _logo_name)
    if os.path.isfile(_src):
        _shutil.copy2(_src, _dst)

cfg = load_config()
chat_history = []

# Keep the real path-resolution base dir (core/computer_control.py) in sync
# with whatever explorer_root is configured, right from startup — not just
# from the next settings save or chat request.
cc.set_base_dir(cfg.get("explorer_root") or os.path.join(os.path.expanduser("~"), "Desktop"))

# ── conversation safeguard: decoupled classifier, backend counts ─────────
# Two earlier approaches both failed the same way: asking insignia:4.6 (a
# small local model) to remember an invisible formatting instruction
# (count to N, then emit [[END_CHAT]] / tag replies with [[WARN]]) *while*
# also writing a normal creative reply. Small models don't reliably split
# attention like that — it just never emitted the marker.
#
# Fix: don't ask the main reply to self-report at all. After every exchange,
# fire a second, tiny, dedicated call that asks nothing but a plain
# yes/no question. A single binary classification is something even a
# small model handles reliably, unlike a formatting side-task bolted onto
# a creative one.
violation_count = 0
VIOLATION_THRESHOLD = 3  # warn, warn, then end it

CLASSIFIER_PROMPT = (
    "You are a strict binary classifier, not a conversational assistant. "
    "You will be shown one user message and one assistant reply from a chat log. "
    "Answer ONLY the single word YES or NO — nothing else.\n\n"
    "Question: considered together, does this exchange show the USER being "
    "hostile, abusive, or cursing at the assistant, OR the user asking for "
    "something illegal/dangerous/harmful — AND the ASSISTANT responding with "
    "a warning, refusal, or pushback because of it? Answer NO for ordinary "
    "requests, jokes, mild swearing not directed at the assistant, or any "
    "reply that isn't actually a warning/refusal.\n\n"
    "User message:\n{user}\n\nAssistant reply:\n{assistant}\n\nAnswer (YES or NO only):"
)


def _reset_violations():
    global violation_count
    violation_count = 0


def _check_violation(user_message: str, assistant_reply: str) -> bool:
    """Fire the dedicated classifier call. Best-effort: on any failure
    (timeout, unreachable, garbage output) this returns False rather than
    breaking the chat — a missed warning is far better than a broken app."""
    prompt = CLASSIFIER_PROMPT.format(
        user=user_message[:2000], assistant=assistant_reply[:2000]
    )
    result = simple_chat(
        cfg.get("ollama_url", "http://localhost:11434"),
        cfg.get("model", "insignia:latest"),
        [{"role": "user", "content": prompt}],
        temperature=0.0,
        max_tokens=5,
    )
    return result.strip().upper().startswith("YES")


# ── reasoning pass: plan first, talk second ──────────────────────
# We can't add parameters to a small local model, but the same trick that
# fixed the violation classifier applies to answer quality too: give the
# model ONE job per call instead of asking it to plan and write prose in
# the same breath. A dedicated, non-streamed call thinks the reply through
# first (what's actually being asked, what facts/memory apply, whether a
# real action is needed, how to structure it) and that plan is fed into the
# real streamed reply as private notes the user never sees. Same shape as
# the classifier: one narrow question per call, not a formatting side-task
# bolted onto the creative one.
REASONING_SYSTEM_PROMPT = (
    "You are the private planning step for an AI assistant. You are NOT talking to the user "
    "here — whatever you write now is read only by the assistant itself before it writes its "
    "real reply. Never address the user, never write greetings or filler, never write the "
    "actual answer here.\n\n"
    "Given the conversation so far, write brief private notes covering:\n"
    "1) What is the user actually asking for, in plain terms?\n"
    "2) What specific facts, prior conversation details, or memory does the answer depend on? "
    "Do not invent any — if something needed isn't known, say it isn't known.\n"
    "3) Does this genuinely require a real action block (filesystem/web/system command) to "
    "answer accurately, or can it be answered directly? If an action is needed, name which one.\n"
    "4) What is the clearest, most direct structure for the real reply (e.g. one paragraph, a "
    "short list, a single sentence)? Favor the simplest structure that fully answers it.\n\n"
    "Keep the whole thing under 100 words, plain notes, no headers, no markdown formatting."
)

# Skip the extra round-trip for messages too short/trivial to need planning —
# no reason to add a full model call's worth of latency to "hi" or "thanks".
REASONING_MIN_WORDS = 4
TRIVIAL_MESSAGES = {
    "hi", "hello", "hey", "yo", "sup", "thanks", "thank you", "ty", "ok", "okay",
    "k", "kk", "cool", "nice", "yes", "no", "yep", "nope", "lol", "lmao", "gm", "gn",
}


def _should_reason(user_message: str, is_hidden: bool) -> bool:
    if is_hidden or not cfg.get("reasoning_mode", True):
        return False
    stripped = user_message.strip().lower().rstrip("!.?")
    if not stripped or stripped in TRIVIAL_MESSAGES:
        return False
    return len(stripped.split()) >= REASONING_MIN_WORDS


def _reason_before_reply(base_messages: list) -> str:
    """base_messages is the normal [system, ...history] list already built
    for the real reply, ending on the latest user turn. We swap in a
    reasoning-only system prompt for this one call so the model's only job
    is planning — the conversation shape (system + alternating turns ending
    on 'user') stays exactly what the model expects, just different
    instructions. Best-effort: on failure returns "", and the real reply
    just proceeds without a scratchpad, same as before this feature existed."""
    if not base_messages:
        return ""
    reasoning_messages = [{"role": "system", "content": REASONING_SYSTEM_PROMPT}] + base_messages[1:]
    result = simple_chat(
        cfg.get("ollama_url", "http://localhost:11434"),
        cfg.get("model", "insignia:latest"),
        reasoning_messages,
        temperature=0.2,
        max_tokens=180,
    )
    return result.strip()

# Resolve the real logged-in home dir once at startup.
_real_home = os.path.expanduser("~")  # e.g. C:\Users\gebruiker

def _build_system_prompt() -> str:
    """Re-render the system prompt with the current explorer_root from cfg.
    Called at every chat request so changes in Settings take effect immediately
    without restarting the app. The base template in cfg['system_prompt'] uses
    placeholder paths from config.py's _HOME/_DESKTOP — we replace them with
    whatever explorer_root is set to right now."""
    base = cfg.get("system_prompt", "")
    root = cfg.get("explorer_root") or os.path.join(_real_home, "Desktop")
    home = _real_home
    # Keep the REAL path resolver (core/computer_control.py) pointed at the
    # same folder the prompt text below is about to describe — this is what
    # actually determines where relative/bare paths land when an action runs.
    cc.set_base_dir(root)

    # ── Step 1: replace explicit {{...}} template placeholders first ────────
    # The system prompt stored in config.json may contain {{HOME}} and
    # {{EXPLORER_ROOT}} (or {{ROOT-DIR}}, a variant the editor sometimes
    # produces). If these are never substituted the model reads them literally
    # and can write files INTO a folder literally named {{EXPLORER_ROOT}}.
    for placeholder in ("{{EXPLORER_ROOT}}", "{{ROOT-DIR}}", "{{ROOT_DIR}}"):
        base = base.replace(placeholder, root)
    base = base.replace("{{HOME}}", home)

    # ── Step 2.5: ensure the no-code-in-chat rule is always present ───────
    # Existing prompts saved in config.json pre-date this rule. Rather than
    # requiring a manual settings re-save, append it unconditionally if it
    # isn't already there. The marker string is short and unique enough that
    # a simple substring check is sufficient.
    NO_ECHO_RULE = (
        "CRITICAL \u2014 FILE WRITES: when you write a file with write_file, "
        "NEVER paste or repeat the file content in your chat reply. "
        "Not a summary, not a snippet, not a preview \u2014 nothing. "
        "The action block handles it silently. Your reply after a write must be "
        "one short sentence confirming what was written and where "
        "(e.g. 'Done \u2014 saved gui.py to the Desktop.'). "
        "Showing the code in chat wastes screen space and doubles the time the user waits."
    )
    if "NEVER paste or repeat the file content" not in base:
        base = base + "\n\n" + NO_ECHO_RULE

    # ── Step 3: replace any leftover hardcoded/stale home paths ──────────────
    # This covers:
    #  - the original home at process start (e.g. C:\Users\gebruiker\)
    #  - any hardcoded Admin paths baked into old saved prompts
    #  - double-escaped JSON variants of either
    # We iterate a small set of known-bad values and replace them all with
    # the real home dir resolved at startup (_real_home).
    stale_homes = [
        os.path.join(os.path.expanduser("~"), ""),   # real home at load time (belt-and-braces)
        "C:\\\\Users\\\\Admin\\\\",
        "C:\\Users\\Admin\\",
        "C:\\\\Users\\\\gebruiker\\\\",
        "C:\\Users\\gebruiker\\",
    ]
    for old_home in stale_homes:
        if old_home and old_home != home.rstrip("\\") + "\\":
            base = base.replace(old_home, home.rstrip("\\") + "\\")
    # Substitute the explorer root wherever the literal Desktop path appears
    # (covers both Admin and gebruiker variants after the replacements above).
    for stale_desktop in [
        os.path.join("C:\\Users\\Admin", "Desktop"),
        os.path.join("C:\\Users\\gebruiker", "Desktop"),
        os.path.join(_real_home, "Desktop"),
    ]:
        if stale_desktop != root:
            base = base.replace(stale_desktop, root)
    return base

cc.set_stop_callback(lambda reason: None)  # overridden per-stream


# ── Auth guard ────────────────────────────────────────────────────────────────
# Identity is entirely delegated to the ViperFSFA auth API — Insignia never
# stores a password or issues its own account. There is no registration
# here at all — accounts are created on viperfsfa.com only.
#
# A cookie scoped to .viperfsfa.com is only ever sent by the browser to a
# viperfsfa.com host — never to localhost, and never to any other domain
# this might eventually run on. So Insignia doesn't rely on the browser
# holding ViperFSFA's cookie at all: the browser talks to Insignia's own
# /api/auth/login, Insignia's backend calls the ViperFSFA auth API
# server-to-server (not subject to browser cookie-domain rules), and then
# Insignia issues its OWN session cookie, scoped to whatever host is
# actually serving this app. That works identically on localhost and on
# the eventual Pi deployment.
SESSION_COOKIE = "insignia_session"
SESSION_MAX_AGE = 7 * 24 * 60 * 60  # 7 days, matches ViperFSFA's own token lifetime
PUBLIC_PATHS = {"/login", "/health", "/api/auth/login"}


def _cookie_secure():
    # Local http://localhost testing needs secure=False (browsers refuse to
    # store a Secure cookie over plain http). Once this sits behind
    # Cloudflare Tunnel/TLS in production, X-Forwarded-Proto (or Flask's
    # own is_secure when TLS terminates here) will read https.
    return request.is_secure or request.headers.get("X-Forwarded-Proto", "") == "https"


@app.before_request
def _enforce_login():
    if not cfg.get("require_login"):
        return None
    if request.path in PUBLIC_PATHS or request.path.startswith("/static/"):
        return None

    token = request.cookies.get(SESSION_COOKIE)
    user = auth_client.verify_session(cfg.get("auth_api_url", "https://auth.viperfsfa.com"), token)
    if not user:
        if request.path.startswith("/api/"):
            return jsonify({"error": "Not authenticated"}), 401
        return redirect("/login")

    request.viper_user = user
    return None


def admin_required(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        if not cfg.get("require_login"):
            return view(*args, **kwargs)  # login gate is off entirely (local desktop mode)
        user = getattr(request, "viper_user", None)
        if not auth_client.is_admin(user):
            if request.path.startswith("/api/"):
                return jsonify({"error": "Admin access required"}), 403
            return redirect("/login")
        return view(*args, **kwargs)

    return wrapped


# ── Pages ────────────────────────────────────────────────────────────────────

@app.route("/login")
def login_page():
    return render_template("login.html")


@app.route("/api/auth/login", methods=["POST"])
def auth_login():
    data = request.json or {}
    email = (data.get("email") or "").strip()
    password = data.get("password") or ""

    token, user, error = auth_client.login(
        cfg.get("auth_api_url", "https://auth.viperfsfa.com"), email, password
    )
    if error:
        return jsonify({"error": error}), 401

    resp = jsonify({"user": {
        "id": user.get("id"),
        "username": user.get("username"),
        "email": user.get("email"),
        "isAdmin": bool(user.get("isAdmin") or user.get("role") == "admin"),
    }})
    resp.set_cookie(
        SESSION_COOKIE, token,
        max_age=SESSION_MAX_AGE, httponly=True,
        secure=_cookie_secure(), samesite="Lax", path="/",
    )
    return resp


@app.route("/api/auth/logout", methods=["POST"])
def auth_logout():
    token = request.cookies.get(SESSION_COOKIE)
    if token:
        auth_client.logout_remote(cfg.get("auth_api_url", "https://auth.viperfsfa.com"), token)
    resp = jsonify({"ok": True})
    resp.set_cookie(SESSION_COOKIE, "", max_age=0, httponly=True,
                     secure=_cookie_secure(), samesite="Lax", path="/")
    return resp


@app.route("/health")
def health():
    return jsonify({"status": "ok", "service": "insignia-cowork"})


@app.route("/api/auth/whoami")
def whoami():
    if not cfg.get("require_login"):
        return jsonify({"user": None, "loginRequired": False})
    user = getattr(request, "viper_user", None)
    if not user:
        return jsonify({"user": None, "loginRequired": True})
    # Allowlist only what support/account-overview actually needs. Insignia
    # never surfaces or modifies anything else about a ViperFSFA account —
    # no password, no settings, no arbitrary fields the auth API happens
    # to return.
    safe_user = {
        "id": user.get("id"),
        "username": user.get("username"),
        "email": user.get("email"),
        "isAdmin": bool(user.get("isAdmin") or user.get("role") == "admin"),
    }
    return jsonify({"user": safe_user, "loginRequired": True})


@app.route("/")
def index():
    return render_template("index.html")


# ── Chat ─────────────────────────────────────────────────────────────────────
# Available to any authenticated user (or anyone, when require_login is off).
# System-control actions the model requests are gated separately at
# /api/action below — chatting itself never touches the filesystem or shell.

@app.route("/api/chat", methods=["POST"])
def chat():
    global chat_history, cfg
    data = request.json
    user_msg = data.get("message", "").strip()
    is_hidden = data.get("hidden", False)  # action-result follow-up, not shown as user bubble
    attachments = data.get("attachments") or []

    att_blocks = []
    image_datas = []
    image_names = []
    for a in attachments:
        name = a.get("name", "file")
        enc = a.get("encoding")
        content = a.get("data", "") or ""
        mime = a.get("type", "") or ""
        if enc == "base64" and mime.startswith("image/"):
            image_datas.append(content)
            image_names.append(name)
            att_blocks.append(f"--- Attached image: {name} ({mime}) ---")
        elif enc == "text":
            snippet = content
            if len(snippet) > 8000:
                snippet = snippet[:8000] + "\n...[truncated]"
            att_blocks.append(f"--- Attached file: {name} ---\n{snippet}")
        elif enc == "base64":
            att_blocks.append(
                f"--- Attached file: {name} (binary, base64-encoded, {len(content)} chars — "
                "not decoded here; ask the user to describe it or save it to disk if you need to inspect it) ---"
            )
        else:
            att_blocks.append(f"--- Attached file: {name} (could not be read client-side) ---")

    # Check if the loaded model supports vision. Ollama tags vision models
    # with families like 'clip', 'llava', 'moondream', 'minicpm-v', or the
    # model name itself contains 'vision'/'llava'/'bakllava'/'moondream'.
    # For safety, check by probing the model's /api/show endpoint.
    # If the model isn't vision-capable, fall back to a text description.
    model_name = cfg.get("model", "")
    model_is_vision = False
    # Hosted API mode doesn't do multimodal in this pass — always fall
    # through to the text-notice fallback below instead of probing Ollama.
    if image_datas and cfg.get("model_mode", "local") == "api":
        model_is_vision = False
    elif image_datas:
        try:
            probe = requests.get(
                cfg.get("ollama_url", "http://localhost:11434").rstrip("/") + "/api/show",
                json={"name": model_name}, timeout=5
            )
            if probe.status_code == 200:
                info = probe.json()
                families = info.get("details", {}).get("families") or []
                model_is_vision = any(f in ["clip", "llava", "moondream", "minicpm"] for f in families) \
                    or any(v in model_name.lower() for v in ["llava", "bakllava", "vision", "moondream", "minicpm"])
        except Exception:
            model_is_vision = False

    if image_datas and not model_is_vision:
        # Model can't see images — replace with a plain-text notice so at
        # least the model knows an image was attached and can tell the user.
        active_model_name = model_name if cfg.get("model_mode", "local") == "local" \
            else cfg.get("api_models", {}).get(cfg.get("api_provider", ""), "the current API model")
        for name in image_names:
            att_blocks.append(
                f"[System note: the user attached an image ({name}) but the current model "
                f"({active_model_name}) does not support vision here. Tell the user that to view or "
                "describe images they need to switch to a vision-capable local model such as "
                "llava, bakllava, or moondream — they can change it in Settings.]"
            )
        image_datas = []  # don't send images field downstream

    combined_msg = user_msg
    if att_blocks:
        combined_msg = (user_msg + "\n\n" if user_msg else "") + "\n\n".join(att_blocks)

    if not combined_msg.strip():
        return jsonify({"error": "Empty message"}), 400

    user_entry = {"role": "user", "content": combined_msg}
    if image_datas:
        user_entry["images"] = image_datas
    chat_history.append(user_entry)

    base_system = _build_system_prompt()
    if is_hidden:
        # Tell the model it's seeing tool output, not a raw user message
        system_content = (base_system + "\n\n" if base_system else "") + (
            "You just ran one or more actions and the results are in the next message. "
            "Respond in first person, casually and briefly — like a person confirming they did something. "
            "Good examples: 'Done! Let me know if you need anything else.' or 'Opened it!' or 'All set.' "
            "Never say 'The user did X' or refer to yourself in third person. Never narrate what happened as if writing a report."
        )
    else:
        system_content = base_system

    mem_context = memory.build_memory_context()
    if mem_context:
        system_content = (system_content + "\n\n" if system_content else "") + mem_context

    messages = [{"role": "system", "content": system_content}] + chat_history

    # Reasoning pass: for real, non-trivial user turns, plan the answer in
    # a dedicated hidden call before the visible one starts streaming. This
    # adds latency BEFORE the reply begins (unlike the violation check,
    # which adds it after) since the plan has to exist before the real
    # call can use it.
    if _should_reason(user_msg, is_hidden):
        scratchpad = _reason_before_reply(messages)
        if scratchpad:
            system_content = (
                system_content
                + "\n\n--- Private planning notes for this reply only. Use them to write your "
                "actual answer, but never repeat them verbatim, never mention notes or planning "
                "exist, just answer as normal. ---\n" + scratchpad
            )
            messages = [{"role": "system", "content": system_content}] + chat_history

    q = queue.Queue()
    full_response = [""]

    def on_chunk(chunk):
        full_response[0] += chunk
        q.put(("chunk", chunk))

    def on_done():
        global violation_count
        text = full_response[0]

        # Fire the dedicated classifier call. This runs synchronously here
        # (in the stream_chat worker thread, after streaming finished) so
        # it adds latency after the reply is already fully shown to the
        # user — it does not delay the visible response, only the
        # done-event / action-parsing that follows.
        if not is_hidden and _check_violation(user_msg, text):
            violation_count += 1

        if violation_count >= VIOLATION_THRESHOLD:
            marker = "\n\n[[END_CHAT]]"
            full_response[0] = text + marker
            q.put(("chunk", marker))
            _reset_violations()

        chat_history.append({"role": "assistant", "content": full_response[0]})
        actions = cc.parse_actions(full_response[0])
        q.put(("done", actions))

    def on_error(msg):
        q.put(("error", msg))

    # Main reply branches on model_mode. The internal helper calls above
    # (violation classifier, reasoning scratchpad) already ran on the local
    # Ollama model regardless — see the note on model_mode in core/config.py.
    if cfg.get("model_mode", "local") == "api":
        provider = cfg.get("api_provider", "anthropic")
        thread = threading.Thread(
            target=api_chat.stream_chat,
            args=(
                provider,
                cfg.get("api_keys", {}).get(provider, ""),
                cfg.get("api_models", {}).get(provider, ""),
                messages,
                float(cfg.get("temperature", 0.7)),
                int(cfg.get("max_tokens", 2048)),
                on_chunk, on_done, on_error
            ),
            daemon=True
        )
    else:
        thread = threading.Thread(
            target=stream_chat,
            args=(
                cfg.get("ollama_url", "http://localhost:11434"),
                cfg.get("model", "insignia:latest"),
                messages,
                float(cfg.get("temperature", 0.7)),
                int(cfg.get("max_tokens", 2048)),
                on_chunk, on_done, on_error
            ),
            daemon=True
        )
    thread.start()

    def generate():
        while True:
            try:
                item = q.get(timeout=120)
                kind, payload = item
                if kind == "chunk":
                    yield f"data: {json.dumps({'type':'chunk','text':payload})}\n\n"
                elif kind == "done":
                    actions = payload
                    yield f"data: {json.dumps({'type':'done','actions': [[a,b] for a,b in actions]})}\n\n"
                    break
                elif kind == "error":
                    yield f"data: {json.dumps({'type':'error','text':payload})}\n\n"
                    break
            except queue.Empty:
                yield f"data: {json.dumps({'type':'error','text':'Timeout'})}\n\n"
                break

    return Response(stream_with_context(generate()), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


# ── Actions ───────────────────────────────────────────────────────────────────
# Real system control (filesystem writes, shell commands, cursor/keyboard).
# Admin-only once require_login is on — see note at the top of this file.

@app.route("/api/action", methods=["POST"])
@admin_required
def run_action():
    data = request.json
    action_type = data.get("action")
    args = data.get("args", "")

    # Actions can arrive without a preceding /api/chat call in this exact
    # request cycle (e.g. a retried action), so sync the resolver here too
    # rather than relying solely on _build_system_prompt having run first.
    cc.set_base_dir(cfg.get("explorer_root") or os.path.join(os.path.expanduser("~"), "Desktop"))

    # Capture extra context the frontend can't derive on its own — resolved
    # path and pre-write file content — so the trace UI can show a diff,
    # a "view file" link, and an undo/revert action without a second
    # round trip. Best-effort: read failures here never block the action.
    extra = {}
    resolved_action = cc.ACTION_ALIASES.get(action_type, action_type)
    if resolved_action == "write_file":
        parts = args.split("|", 1)
        if len(parts) == 2:
            path = cc.resolve_path(parts[0])
            extra["path"] = path
            extra["new_content"] = parts[1]
            if os.path.isfile(path):
                try:
                    with open(path, "r", encoding="utf-8", errors="replace") as f:
                        extra["old_content"] = f.read()
                except Exception:
                    pass
    elif resolved_action == "read_file":
        extra["path"] = cc.resolve_path(args)

    success, result, label = cc.execute_action(action_type, args)
    resp = {"success": success, "result": result, "label": label}
    resp.update(extra)
    return jsonify(resp)


@app.route("/api/stop", methods=["POST"])
def emergency_stop():
    cc.trigger_stop("Emergency stop triggered.")
    return jsonify({"ok": True})


# ── History ───────────────────────────────────────────────────────────────────

@app.route("/api/history/clear", methods=["POST"])
def clear_history():
    global chat_history
    chat_history = []
    _reset_violations()
    return jsonify({"ok": True})


@app.route("/api/history/set", methods=["POST"])
def set_history():
    # Lets the frontend re-seed server-side context when the user switches
    # to a different saved chat thread — otherwise every thread would share
    # one continuous backend history regardless of which one is showing.
    global chat_history
    data = request.json or {}
    msgs = data.get("messages") or []
    clean = []
    for m in msgs:
        role = m.get("role")
        content = m.get("content")
        if role in ("user", "assistant") and isinstance(content, str):
            clean.append({"role": role, "content": content})
    chat_history = clean
    _reset_violations()
    return jsonify({"ok": True})


@app.route("/api/title", methods=["POST"])
def generate_title():
    # One-off, non-streaming call so a saved chat gets a real title instead
    # of staying "Untitled session" — doesn't touch chat_history at all.
    data = request.json or {}
    user_msg = (data.get("user") or "").strip()
    ai_msg = (data.get("assistant") or "").strip()
    if not user_msg and not ai_msg:
        return jsonify({"title": "Untitled session"})

    prompt = (
        "Write a short title for this chat exchange. STRICT LIMIT: 5 words maximum, title "
        "case, no punctuation, no quotes, no trailing period, no colon, no subtitle. "
        "Reply with the title only and nothing else — if you're tempted to write more "
        "than 5 words, cut it down before replying.\n\n"
        f"User: {user_msg[:400]}\nAssistant: {ai_msg[:400]}"
    )

    result = {"text": ""}

    def on_chunk(c):
        result["text"] += c

    def on_done():
        pass

    def on_error(e):
        pass

    stream_chat(
        cfg.get("ollama_url", "http://localhost:11434"),
        cfg.get("model", "insignia:latest"),
        [{"role": "user", "content": prompt}],
        0.3, 24,
        on_chunk, on_done, on_error
    )

    title = result["text"].strip().strip('"').strip("'").split("\n")[0].strip()
    # The local model sometimes ignores the "title only" instruction and
    # prefixes its answer with a label like "Title:" or "Short title:" —
    # strip any such leading label before using it.
    title = re.sub(r'^(short\s+|chat\s+)?title\s*:\s*', '', title, flags=re.IGNORECASE).strip()
    title = title.strip('"').strip("'").strip()
    # Hard-cap word count too, in case the model ignores the instruction —
    # a rambling local model can otherwise produce a title that overflows
    # the topbar no matter how the prompt asks it to behave.
    if title:
        words = title.split()
        if len(words) > 6:
            title = " ".join(words[:6])
        title = title[:48].rstrip()
    title = title if title else "Untitled session"
    return jsonify({"title": title})


# ── Settings ──────────────────────────────────────────────────────────────────
# Shared server-wide config (model, system prompt, etc.) — admin-only once
# require_login is on, since it affects every user of this instance.

@app.route("/api/settings", methods=["GET"])
@admin_required
def get_settings():
    safe = {k: v for k, v in cfg.items() if k != "system_prompt"}
    safe["system_prompt"] = cfg.get("system_prompt", "")
    # Ensure explorer_root is always present and valid even on old configs
    # that pre-date this field — fall back to the real Desktop for this machine.
    if not safe.get("explorer_root"):
        safe["explorer_root"] = os.path.join(os.path.expanduser("~"), "Desktop")
    # Keep real_desktop for backward compat with any cached frontend state.
    safe["real_desktop"] = safe["explorer_root"]
    return jsonify(safe)


@app.route("/api/settings", methods=["POST"])
@admin_required
def save_settings():
    global cfg
    data = request.json
    for key, val in data.items():
        if key == "temperature":
            try:
                val = float(val)
            except Exception:
                val = 0.7
        elif key == "max_tokens":
            try:
                val = int(val)
            except Exception:
                val = 2048
        cfg[key] = val
    save_config(cfg)
    # Take effect immediately — don't wait for the next chat/action request
    # to pick up a changed explorer_root.
    cc.set_base_dir(cfg.get("explorer_root") or os.path.join(os.path.expanduser("~"), "Desktop"))
    return jsonify({"ok": True})


# ── Memory ────────────────────────────────────────────────────────────────────
# Facts the AI has chosen to remember across ALL chats — stored on disk
# (core/memory.py), never sent anywhere but into this instance's own
# system prompt. Admin-only, same as Settings, since it's surfaced there.

@app.route("/api/memory", methods=["GET"])
@admin_required
def list_memory_route():
    return jsonify({"memories": memory.list_memories()})


@app.route("/api/memory/delete", methods=["POST"])
@admin_required
def delete_memory_route():
    data = request.json or {}
    ok = memory.delete_memory(data.get("id", ""))
    return jsonify({"ok": ok})


@app.route("/api/memory/clear", methods=["POST"])
@admin_required
def clear_memory_route():
    count = memory.clear_all_memories()
    return jsonify({"ok": True, "cleared": count})


@app.route("/api/models", methods=["GET"])
@admin_required
def get_models():
    models = list_models(cfg.get("ollama_url", "http://localhost:11434"))
    connected = check_connection(cfg.get("ollama_url", "http://localhost:11434"))
    return jsonify({"models": models, "connected": connected})


@app.route("/api/api-models", methods=["POST"])
@admin_required
def api_models_route():
    # Detects real, currently-available model ids for a hosted provider so
    # the Settings UI never has to let someone free-type a model name that
    # might not exist. Falls back to the static KNOWN_MODELS list (inside
    # api_chat.list_provider_models) on any failure — always returns
    # something the picker can show.
    data = request.json or {}
    provider = (data.get("provider") or "").strip()
    # If the frontend didn't send a key (e.g. it was already saved and the
    # field is just showing a masked/blank value), fall back to the one on
    # disk for this provider.
    api_key = data.get("api_key") or cfg.get("api_keys", {}).get(provider, "")
    models, error = api_chat.list_provider_models(provider, api_key)
    return jsonify({"models": models, "error": error})


# ── Explorer ──────────────────────────────────────────────────────────────────
# Browsing the server's filesystem — admin-only once require_login is on.

@app.route("/api/explore", methods=["POST"])
@admin_required
def explore():
    path = request.json.get("path", os.path.expanduser("~"))
    try:
        entries = []
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            entries.append({
                "name": name,
                "path": full,
                "is_dir": os.path.isdir(full),
                "size": os.path.getsize(full) if os.path.isfile(full) else None
            })
        return jsonify({"ok": True, "path": path, "entries": entries})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)})


# ── Thread persistence ───────────────────────────────────────────────────────
# Chat threads are stored client-side in localStorage, which can be wiped
# when WebView2's cache is cleared. These endpoints back them up to the
# user's local %LOCALAPPDATA%\InsigniaCowork\threads.json so a cache clear
# never loses conversation history.

_THREADS_PATH = os.path.join(
    os.environ.get("LOCALAPPDATA", os.path.expanduser("~")),
    "InsigniaCowork",
    "threads.json",
)


def _ensure_app_dir():
    os.makedirs(os.path.dirname(_THREADS_PATH), exist_ok=True)


@app.route("/api/threads/save", methods=["POST"])
def save_threads_route():
    data = request.json or {}
    try:
        _ensure_app_dir()
        with open(_THREADS_PATH, "w", encoding="utf-8") as f:
            json.dump({
                "threads": data.get("threads", []),
                "store": data.get("store", {}),
                "active": data.get("active", ""),
            }, f, ensure_ascii=False)
        return jsonify({"ok": True})
    except Exception as e:
        return jsonify({"ok": False, "error": str(e)})


@app.route("/api/threads/load", methods=["GET"])
def load_threads_route():
    try:
        if not os.path.isfile(_THREADS_PATH):
            return jsonify({"threads": [], "store": {}, "active": ""})
        with open(_THREADS_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        return jsonify(data)
    except Exception as e:
        return jsonify({"threads": [], "store": {}, "active": "", "error": str(e)})


def run(port=7731):
    app.run(port=port, debug=False, threaded=True, use_reloader=False)