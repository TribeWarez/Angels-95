#include "LightningScriptContext.hpp"
#include <cstring>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// Load — split text into lines, build jump label index
// ---------------------------------------------------------------------------
bool LightningScriptContext::Load(const std::string& scriptText) {
    m_lines.clear();
    m_intVars.clear();
    m_floatVars.clear();
    m_strVars.clear();
    m_jumpLabels.clear();
    m_pendingStatOps.clear();
    std::memset(m_flags, 0, sizeof(m_flags));
    m_pc = 0;

    std::istringstream stream(scriptText);
    std::string line;
    int lineIdx = 0;
    while (std::getline(stream, line)) {
        // Trim
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { m_lines.push_back(""); lineIdx++; continue; }
        size_t end = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(start, end - start + 1);

        // Skip comments
        if (trimmed[0] == '#' || trimmed.rfind("//", 0) == 0) {
            m_lines.push_back("");
            lineIdx++;
            continue;
        }

    m_lines.push_back(trimmed);
    lineIdx++;
        }

    // Build jump label index immediately
    for (int i = 0; i < lineIdx; i++) {
        const std::string& ln = m_lines[i];
        size_t colon = ln.find(':');
        if (colon != std::string::npos && colon > 0) {
            bool isLabel = true;
            for (size_t c = 0; c < colon; c++) {
                if (!std::isalnum(ln[c]) && ln[c] != '_') { isLabel = false; break; }
            }
            if (isLabel && (colon + 1 >= ln.size() || ln[colon+1] == ' ')) {
                m_jumpLabels[ln.substr(0, colon)] = i;
            }
        }
    }

    return true;
}

int LightningScriptContext::FindJumpLabel(const std::string& label) const {
    auto it = m_jumpLabels.find(label);
    if (it != m_jumpLabels.end()) return it->second;
    return -1;
}

// ---------------------------------------------------------------------------
// FindNextJumpLabel — nearest action label strictly after a line index
// ---------------------------------------------------------------------------
int LightningScriptContext::FindNextJumpLabel(int afterLine) const {
    int best = -1;
    for (auto& [label, line] : m_jumpLabels) {
        (void)label;
        if (line > afterLine && (best < 0 || line < best)) best = line;
    }
    return best;
}

// ---------------------------------------------------------------------------
// RunAction — execute one action block without overrunning into the next one
// ---------------------------------------------------------------------------
int LightningScriptContext::RunAction(const std::string& actionName, int maxSteps) {
    int labelLine = FindJumpLabel(actionName);
    if (labelLine < 0) return 0;
    Reset();
    // Jump directly past the action label — never execute other actions' bodies
    m_pc = labelLine + 1;
    int nextLabel = FindNextJumpLabel(labelLine);
    int steps = 0;
    while (steps < maxSteps && HasMore()) {
        if (nextLabel >= 0 && m_pc >= nextLabel) break;
        if (!ExecuteNext()) break;
        steps++;
    }
    // Exhaust the script so the per-frame tick in LightningEntityManager::Update
    // cannot keep executing the leftover bodies of other actions.
    m_pc = (int)m_lines.size();
    return steps;
}

// ---------------------------------------------------------------------------
// Small text helpers shared by the Phase-1 opcodes + EvalCondition.
// ---------------------------------------------------------------------------
static std::string StripQuotes(std::string s) {
    size_t a = s.find_first_not_of(" \t\"");
    size_t b = s.find_last_not_of(" \t\"\r\n");
    if (a == std::string::npos || b == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

// True when s is exactly one balanced (...) group: "(a && b)" yes,
// "(a) && (b)" no. The naive front/back check used to mangle the latter.
static bool EnclosesWhole(const std::string& s) {
    if (s.size() < 2 || s.front() != '(' || s.back() != ')') return false;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') {
            depth--;
            if (depth == 0 && i != s.size() - 1) return false;
        }
    }
    return depth == 0;
}

// First index of `tok` at paren depth 0, or npos.
static size_t FindTopLevel(const std::string& s, const std::string& tok) {
    int depth = 0;
    for (size_t i = 0; i + tok.size() <= s.size(); ++i) {
        if (s[i] == '(') { depth++; continue; }
        if (s[i] == ')') { depth--; continue; }
        if (depth == 0 && s.compare(i, tok.size(), tok) == 0) return i;
    }
    return std::string::npos;
}

// ---------------------------------------------------------------------------
// ExecuteNext — run one instruction at m_pc, advance if not a jump
// ---------------------------------------------------------------------------
bool LightningScriptContext::ExecuteNext() {
    if (m_pc < 0 || m_pc >= (int)m_lines.size()) return false;

    const std::string& line = m_lines[m_pc];
    if (line.empty()) { m_pc++; return true; }

    // Check for label (skip execution, just advance)
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        bool allAlpha = true;
        for (size_t c = 0; c < colon; c++) { if (!std::isalnum(line[c]) && line[c] != '_') { allAlpha = false; break; } }
        if (allAlpha && (colon + 1 >= line.size() || line[colon+1] == ' ')) {
            m_pc++;
            return true;
        }
    }

    std::istringstream ls(line);
    std::string opcode;
    ls >> opcode;

    // -- Variable operations --
    if (opcode == "var") {
        // var name = value  (auto-detect int vs float)
        std::string name, eq;
        std::string valueStr;
        ls >> name >> eq;
        if (eq != "=") { m_pc++; return true; }
        ls >> valueStr;
        try {
            if (valueStr.find('.') != std::string::npos)
                m_floatVars[name] = std::stof(valueStr);
            else
                m_intVars[name] = std::stoi(valueStr);
        } catch (const std::exception& e) {
            LS_WARN("Failed to parse value for '%s': %s", name.c_str(), e.what());
            m_intVars[name] = 0;
        }
        m_pc++;

    } else if (opcode.rfind("$", 0) == 0) {
        // $var = value or $var += value etc.
        std::string name = opcode.substr(1);
        std::string op;
        ls >> op;
        std::string valueStr;
        std::getline(ls, valueStr);
        // Trim valueStr
        size_t vs = valueStr.find_first_not_of(" \t");
        if (vs != std::string::npos) valueStr = valueStr.substr(vs);
        size_t ve = valueStr.find_last_not_of(" \t\r\n");
        if (ve != std::string::npos) valueStr = valueStr.substr(0, ve + 1);

        auto getVal = [&](const std::string& s) -> float {
            if (s.rfind("$", 0) == 0) return ResolveVar(s.substr(1));
            try { return std::stof(s); } catch (...) { return 0; }
        };

        // Minimal expression support: "A op B" (single binary op, left-to-right,
        // operands are variables or numbers). A lone token behaves as before.
        auto evalExpr = [&](const std::string& expr) -> float {
            std::istringstream es(expr);
            std::string a, opTok, b;
            if (!(es >> a)) return 0;
            float lhs = getVal(a);
            if (!(es >> opTok)) return lhs;
            if (!(es >> b)) return lhs;
            float rhs = getVal(b);
            if (opTok == "+") return lhs + rhs;
            if (opTok == "-") return lhs - rhs;
            if (opTok == "*") return lhs * rhs;
            if (opTok == "/") return (rhs != 0.0f) ? lhs / rhs : 0.0f;
            return lhs; // unknown operator — plain var read
        };

        float val = evalExpr(valueStr);

        if (op == "=") {
            if (valueStr.find('.') != std::string::npos || m_floatVars.count(name))
                m_floatVars[name] = val;
            else
                m_intVars[name] = (int)val;
        } else if (op == "+=") {
            if (m_floatVars.count(name)) m_floatVars[name] += val;
            else m_intVars[name] = m_intVars[name] + (int)val;
        } else if (op == "-=") {
            if (m_floatVars.count(name)) m_floatVars[name] -= val;
            else m_intVars[name] = m_intVars[name] - (int)val;
        } else if (op == "*=") {
            if (m_floatVars.count(name)) m_floatVars[name] *= val;
            else m_intVars[name] = m_intVars[name] * (int)val;
        } else if (op == "/=") {
            if (val != 0.0f) {
                if (m_floatVars.count(name)) m_floatVars[name] /= val;
                else m_intVars[name] = m_intVars[name] / (int)val;
            }
        }
        m_pc++;

    } else if (opcode == "if") {
        std::string cond;
        std::getline(ls, cond);
        // Trim and strip surrounding parentheses
        size_t cs = cond.find_first_not_of(" \t");
        if (cs != std::string::npos) cond = cond.substr(cs);
        size_t ce = cond.find_last_not_of(" \t\r\n");
        if (ce != std::string::npos) cond = cond.substr(0, ce + 1);
        // Brace-style scripts keep the trailing '{' glued to the condition —
        // strip it so `if ($x == 0) {` parses identically to `if ($x == 0)`.
        while (!cond.empty() && (cond.back() == '{' || cond.back() == '}'))
            cond.pop_back();
        // Trim again after brace strip
        ce = cond.find_last_not_of(" \t\r\n");
        if (ce != std::string::npos) cond = cond.substr(0, ce + 1);
        // Strip a balanced (...) group wrapping the whole condition. The old
        // naive front/back strip mangled "(a) && (b)" into "a) && (b".
        while (EnclosesWhole(cond)) {
            cond = cond.substr(1, cond.size() - 2);
            size_t s2 = cond.find_first_not_of(" \t");
            if (s2 != std::string::npos) cond = cond.substr(s2);
            size_t e2 = cond.find_last_not_of(" \t\r\n");
            if (e2 != std::string::npos) cond = cond.substr(0, e2 + 1);
        }

        if (!EvalCondition(cond)) {
            // Skip if-body; stop before else or at endif.
            // Brace-style scripts close blocks with '}' instead of 'endif' —
            // a '}'-led line decrements depth the same way (so a false
            // condition resumes AFTER its closing brace, not at script end).
            // '{'-led lines are no-ops; '} else {' lands in the else body.
            int depth = 1;
            m_pc++;
            bool foundElse = false;
            while (m_pc < (int)m_lines.size() && depth > 0) {
                const std::string& sl = m_lines[m_pc];
                size_t fs = sl.find_first_not_of(" \t");
                if (fs != std::string::npos && sl[fs] == '}') {
                    if (sl.find("else", fs) != std::string::npos && depth == 1) {
                        foundElse = true; break; // '} else {' → run the else body
                    }
                    depth--;                     // block closer — like endif
                }
                else if (fs != std::string::npos && sl[fs] == '{') {
                    // block opener on its own line — no depth change
                }
                else if (sl.rfind("if", 0) == 0 && sl.size() > 2 && std::isblank(sl[2])) depth++;
                else if (sl.rfind("endif", 0) == 0) depth--;
                else if (sl.rfind("else", 0) == 0 && depth == 1) { foundElse = true; break; }
                if (depth > 0) m_pc++;
            }
            if (foundElse) {
                // Land past the else line so the else-body executes
                m_pc++;
                if (m_pc < (int)m_lines.size() && m_lines[m_pc].rfind("endif", 0) == 0)
                    m_pc = (int)m_lines.size(); // empty else block
            } else {
                if (m_pc < (int)m_lines.size()) m_pc++;
            }
            return true;
        }
        m_pc++;
        return true;

    } else if (opcode == "else") {
        // Skip to endif ('}'-led lines close blocks like endif; '{' no-op)
        int depth = 1;
        m_pc++;
        while (m_pc < (int)m_lines.size() && depth > 0) {
            const std::string& sl = m_lines[m_pc];
            size_t fs = sl.find_first_not_of(" \t");
            if (fs != std::string::npos && sl[fs] == '}') depth--;
            else if (fs != std::string::npos && sl[fs] == '{') {
                // block opener on its own line — no depth change
            }
            else if (sl.rfind("if", 0) == 0 && sl.size() > 2 && std::isblank(sl[2])) depth++;
            else if (sl.rfind("endif", 0) == 0) depth--;
            if (depth > 0) m_pc++;
        }
        if (m_pc < (int)m_lines.size()) m_pc++;
        return true;

    } else if (opcode == "endif") {
        m_pc++;

    } else if (opcode == "goto" || opcode == "jump") {
        std::string label;
        ls >> label;
        auto it = m_jumpLabels.find(label);
        if (it != m_jumpLabels.end()) m_pc = it->second;
        else m_pc++;

    } else if (opcode == "say") {
        std::string msg;
        std::getline(ls, msg);
        // Trim quotes if present
        size_t qs = msg.find_first_not_of(" \t\"");
        size_t qe = msg.find_last_not_of(" \t\"\r\n");
        if (qs != std::string::npos && qe != std::string::npos)
            msg = msg.substr(qs, qe - qs + 1);
        LS_LOG("say: %s", msg.c_str());
        fprintf(stdout, "[LightningScript] %s\n", msg.c_str());
        m_pc++;

    } else if (opcode == "stop" || opcode == "end") {
        m_pc = (int)m_lines.size(); // Halt execution

    } else if (opcode == "wtflag") {
        int idx, val;
        ls >> idx >> val;
        if (idx >= 0 && idx < MAX_FLAGS) m_flags[idx] = val;
        m_pc++;

    } else if (opcode == "toggle_flag") {
        int idx;
        ls >> idx;
        if (idx >= 0 && idx < MAX_FLAGS) m_flags[idx] = m_flags[idx] ? 0 : 1;
        m_pc++;

    } else if (opcode == "rtflag") {
        // rtflag idx → store flag into $result
        std::string idxStr;
        ls >> idxStr;
        int idx = 0;
        if (idxStr.rfind("$", 0) == 0) idx = (int)m_floatVars[idxStr.substr(1)];
        else idx = std::stoi(idxStr);
        if (idx >= 0 && idx < MAX_FLAGS) m_intVars["result"] = m_flags[idx];
        m_pc++;

    } else if (opcode == "set_cooldown") {
        float sec;
        ls >> sec;
        if (sec > 0) SetFloat("__cooldown", sec);
        m_pc++;

    } else if (opcode == "play_sound") {
        std::string name;
        std::getline(ls, name);
        {
            size_t a = name.find_first_not_of(" \t\"");
            size_t b = name.find_last_not_of(" \t\"\r\n");
            if (a != std::string::npos && b != std::string::npos) name = name.substr(a, b - a + 1);
        }
        // Sound playback handled by caller via message queue; log for now
        SetStr("__last_sound", name);
        m_pc++;

    } else if (opcode == "set_fog") {
        float r, g, b, d;
        ls >> r >> g >> b >> d;
        SetFloat("__fog_r", r); SetFloat("__fog_g", g);
        SetFloat("__fog_b", b); SetFloat("__fog_density", d);
        m_pc++;

    } else if (opcode == "restore_fog") {
        // Signals the host to revert to the world's fog. These used to erase
        // __fog_* directly, but the host's PopPendingFog() checks for __fog_r to
        // decide whether anything is pending - so erasing it made the whole
        // opcode a silent no-op and fog was never restored.
        SetFloat("__fog_restore", 1.0f);
        m_pc++;

    } else if (opcode == "set_ambient") {
        float r, g, b;
        ls >> r >> g >> b;
        SetFloat("__ambient_r", r); SetFloat("__ambient_g", g);
        SetFloat("__ambient_b", b);
        m_pc++;

    } else if (opcode == "restore_ambient") {
        // See restore_fog: erasing the pending vars made this a no-op.
        SetFloat("__ambient_restore", 1.0f);
        m_pc++;

    } else if (opcode == "set_skybox") {
        std::string name;
        ls >> name;
        {
            size_t a = name.find_first_not_of(" \t\"");
            size_t b = name.find_last_not_of(" \t\"\r\n");
            if (a != std::string::npos && b != std::string::npos) name = name.substr(a, b - a + 1);
        }
        SetStr("__skybox", name);
        m_pc++;

    } else if (opcode == "restore_skybox") {
        // Was SetStr("__skybox", ""), but PopPendingSkybox() returns early on an
        // empty name, so the host skipped it and the skybox never reverted.
        SetStr("__skybox_restore", "1");
        m_pc++;

    } else if (opcode == "spawn_pawn") {
        // spawn_pawn "name" x y z
        std::string name;
        ls >> name;
        {
            size_t a = name.find_first_not_of(" \t\"");
            size_t b = name.find_last_not_of(" \t\"\r\n");
            if (a != std::string::npos && b != std::string::npos) name = name.substr(a, b - a + 1);
        }
        float x, y, z;
        ls >> x >> y >> z;
        SetStr("__pawn_name", name);
        SetFloat("__pawn_x", x);
        SetFloat("__pawn_y", y);
        SetFloat("__pawn_z", z);
        m_pc++;

    } else if (opcode == "msg") {
        std::string msg;
        std::getline(ls, msg);
        size_t qs = msg.find_first_not_of(" \t\"");
        size_t qe = msg.find_last_not_of(" \t\"\r\n");
        if (qs != std::string::npos && qe != std::string::npos)
            msg = msg.substr(qs, qe - qs + 1);
        SetStr("__last_msg", msg);
        m_pc++;

    } else if (opcode == "heal" || opcode == "damage") {
        float amt;
        ls >> amt;
        m_pendingStatOps.push_back({"health", (opcode == "heal") ? 1 : 2, amt});
        if (opcode == "damage") SetFloat("__last_hurt", 1.0f);
        m_pc++;

    } else if (opcode == "playerstat") {
        // playerstat <name> <op> <value>   (op: =, +=, -=, *=, /=)
        std::string name, op2;
        std::string valueStr;
        ls >> name >> op2;
        std::getline(ls, valueStr);
        {
            size_t vs = valueStr.find_first_not_of(" \t");
            if (vs != std::string::npos) valueStr = valueStr.substr(vs);
            size_t ve = valueStr.find_last_not_of(" \t\r\n");
            if (ve != std::string::npos) valueStr = valueStr.substr(0, ve + 1);
        }
        if (!name.empty() && name[0] == '$') name.erase(0, 1);
        float value = 0.0f;
        if (valueStr.rfind("$", 0) == 0) {
            value = ResolveVar(valueStr.substr(1));
        } else {
            try { value = std::stof(valueStr); } catch (...) { value = 0.0f; }
        }
        int stOp = 0;
        if (op2 == "=") stOp = 0;
        else if (op2 == "+=") stOp = 1;
        else if (op2 == "-=") stOp = 2;
        else if (op2 == "*=") stOp = 3;
        else if (op2 == "/=") stOp = 4;
        m_pendingStatOps.push_back({name, stOp, value});
        m_pc++;

    } else if (opcode == "consume") {
        SetFloat("__consume", 1.0f);
        m_pc++;

    } else if (opcode == "spawn_pickup") {
        // spawn_pickup "name" x y z [respawnTime]
        std::string name;
        ls >> name;
        {
            size_t a = name.find_first_not_of(" \t\"");
            size_t b = name.find_last_not_of(" \t\"\r\n");
            if (a != std::string::npos && b != std::string::npos) name = name.substr(a, b - a + 1);
        }
        float x, y, z, respawn = 30.0f;
        ls >> x >> y >> z;
        // Only overwrite the default if a 4th value was actually present. A
        // failed `>>` leaves the target untouched in C++11+ but this stream is
        // reused, and omitting the argument used to yield 0.0 instead of the
        // documented 30.0 - which meant "never respawn" for an omitted value.
        float maybeRespawn = 0.0f;
        if (ls >> maybeRespawn) respawn = maybeRespawn;
        SetStr("__pickup_name", name);
        SetFloat("__pickup_x", x);
        SetFloat("__pickup_y", y);
        SetFloat("__pickup_z", z);
        SetFloat("__pickup_respawn", respawn);
        m_pc++;

    } else if (opcode == "emit_event") {
        // emit_event "topic" [int]  → EventBus::Emit (host drain)
        ScriptCommand c; c.op = "event";
        std::string topic; ls >> topic;
        c.s0 = StripQuotes(topic);
        int iv = 0; if (ls >> iv) c.i0 = iv;
        m_commands.push_back(c);
        m_pc++;

    } else if (opcode == "objective") {
        // objective <id> <op> <value>   op: =, +=, -=
        ScriptCommand c; c.op = "objective";
        std::string id, o; ls >> id >> o;
        float v = 0.0f; ls >> v;
        c.s0 = id;
        c.i0 = (o == "=") ? 0 : (o == "-=") ? 2 : 1;   // default +=
        c.f0 = v;
        m_commands.push_back(c);
        m_pc++;

    } else if (opcode == "set_worldflag") {
        // set_worldflag <name> <value>
        ScriptCommand c; c.op = "worldflag_set";
        std::string name; float v = 0.0f; ls >> name >> v;
        c.s0 = name; c.i0 = (int)v;
        m_commands.push_back(c);
        m_pc++;

    } else if (opcode == "get_worldflag") {
        // get_worldflag <name> → $result
        std::string name; ls >> name;
        m_intVars["result"] = m_worldFlagGetter ? m_worldFlagGetter(name) : 0;
        m_pc++;

    } else if (opcode == "give_xp") {
        // give_xp <amount>
        ScriptCommand c; c.op = "xp"; float v = 0.0f; ls >> v; c.f0 = v;
        m_commands.push_back(c);
        m_pc++;

    } else if (opcode == "teleport") {
        // teleport <x> <y> <z>  (Y-up engine space)
        ScriptCommand c; c.op = "teleport"; ls >> c.x >> c.y >> c.z;
        m_commands.push_back(c);
        m_pc++;

    } else if (opcode == "change_world") {
        // change_world "World"
        ScriptCommand c; c.op = "change_world";
        std::string w; ls >> w; c.s0 = StripQuotes(w);
        m_commands.push_back(c);
        m_pc++;

    } else if (opcode == "grant_item") {
        // grant_item "Def" [qty]
        ScriptCommand c; c.op = "grant_item";
        std::string n; ls >> n; c.s0 = StripQuotes(n);
        int q = 1; if (ls >> q) c.i0 = q; else c.i0 = 1;
        m_commands.push_back(c);
        m_pc++;

    } else if (opcode == "apply_area_damage") {
        // apply_area_damage <radius> <damage>
        ScriptCommand c; c.op = "area_damage";
        float r = 0.0f, d = 0.0f; ls >> r >> d;
        c.f0 = r; c.f1 = d;
        m_commands.push_back(c);
        m_pc++;

    } else if (opcode == "spawn_projectile") {
        // spawn_projectile <speed> <damage> <lifetime> [count] [spread]
        ScriptCommand c; c.op = "projectile";
        float sp = 20.0f, dmg = 10.0f, life = 2.0f, spread = 0.0f;
        int count = 1;
        ls >> sp >> dmg >> life;
        if (!(ls >> count)) count = 1;
        if (!(ls >> spread)) spread = 0.0f;
        c.x = sp; c.y = dmg; c.z = life; c.f0 = (float)count; c.f1 = spread;
        m_commands.push_back(c);
        m_pc++;

    } else if (opcode == "play_clip") {
        // play_clip "Clip"
        ScriptCommand c; c.op = "clip";
        std::string cl; ls >> cl; c.s0 = StripQuotes(cl);
        m_commands.push_back(c);
        m_pc++;

    } else if (opcode == "}" || opcode == "{") {
        // Bare block delimiter (brace-style scripts) — no-op
        m_pc++;

    } else {
        // Fall back to registered game-specific opcode handlers.
        auto it = m_opcodeHandlers.find(opcode);
        if (it != m_opcodeHandlers.end() && it->second) {
            std::istringstream ls(line);
            std::string tok;
            ls >> tok; // skip opcode token
            if (it->second(ls)) {
                // Handler consumed the opcode; pc already advanced or not.
            } else {
                m_pc++;
            }
        } else {
            OZ_WARN("LightningScript: unknown opcode '%s' at line %d", opcode.c_str(), m_pc);
            m_pc++;
        }
    }

    return m_pc < (int)m_lines.size();
}

void LightningScriptContext::RegisterOpcode(const std::string& name, OpcodeHandler handler) {
    if (!handler) return;
    m_opcodeHandlers[name] = std::move(handler);
}

void LightningScriptContext::Reset() {
    m_pc = 0;
}

// ---------------------------------------------------------------------------
// Variable I/O
// ---------------------------------------------------------------------------
void LightningScriptContext::SetInt(const std::string& name, int val) { m_intVars[name] = val; }
int  LightningScriptContext::GetInt(const std::string& name, int defaultVal) const {
    auto it = m_intVars.find(name);
    return (it != m_intVars.end()) ? it->second : defaultVal;
}
void LightningScriptContext::SetFloat(const std::string& name, float val) { m_floatVars[name] = val; }
float LightningScriptContext::GetFloat(const std::string& name, float defaultVal) const {
    auto it = m_floatVars.find(name);
    return (it != m_floatVars.end()) ? it->second : defaultVal;
}
float LightningScriptContext::TakePendingFloat(const std::string& name) {
    auto it = m_floatVars.find(name);
    if (it == m_floatVars.end()) return 0.0f;
    const float v = it->second;
    m_floatVars.erase(it);
    return v;
}
void LightningScriptContext::SetStr(const std::string& name, const std::string& val) { m_strVars[name] = val; }
std::string LightningScriptContext::GetStr(const std::string& name, const std::string& defaultVal) const {
    auto it = m_strVars.find(name);
    return (it != m_strVars.end()) ? it->second : defaultVal;
}

void LightningScriptContext::SetFlag(int idx, int val) {
    if (idx >= 0 && idx < MAX_FLAGS) m_flags[idx] = val;
}
int LightningScriptContext::GetFlag(int idx) const {
    if (idx >= 0 && idx < MAX_FLAGS) return m_flags[idx];
    return 0;
}

// ---------------------------------------------------------------------------
// ResolveVar — shared $-name resolution for assignments, playerstat and
// conditions. Order: float vars → int vars → toggle flags → stat resolver → 0.
// "flag<idx>" (digits only) reads the per-instance toggle flags, so
// `wtflag 0 1` + `if ($flag0 >= 1)` round-trip within one instance.
// ---------------------------------------------------------------------------
float LightningScriptContext::ResolveVar(const std::string& vn) const {
    auto it = m_floatVars.find(vn);
    if (it != m_floatVars.end()) return it->second;
    auto it2 = m_intVars.find(vn);
    if (it2 != m_intVars.end()) return (float)it2->second;
    if (vn.rfind("flag", 0) == 0) {
        std::string num = vn.substr(4);
        if (!num.empty() && std::all_of(num.begin(), num.end(),
                [](unsigned char c) { return std::isdigit(c); })) {
            int idx = std::stoi(num);
            if (idx >= 0 && idx < MAX_FLAGS) return (float)m_flags[idx];
        }
    }
    if (m_statResolver) return m_statResolver(vn);
    return 0;
}

// ---------------------------------------------------------------------------
// EvalCondition — boolean condition parser.
// Supports && / || (left-associative, parentheses), then a single comparison:
//   $var == val, $var != val, > < >= <=
// ---------------------------------------------------------------------------
bool LightningScriptContext::EvalCondition(const std::string& rawCond) {
    std::string cond = rawCond;
    auto trim = [](std::string& s) {
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t\r\n");
        if (a != std::string::npos && b != std::string::npos) s = s.substr(a, b - a + 1);
    };
    trim(cond);
    if (cond.empty()) return false;

    // Strip a balanced (...) group wrapping the whole expression.
    while (EnclosesWhole(cond)) { cond = cond.substr(1, cond.size() - 2); trim(cond); }
    if (cond.empty()) return false;

    // Lowest precedence first.
    size_t p = FindTopLevel(cond, "||");
    if (p != std::string::npos)
        return EvalCondition(cond.substr(0, p)) || EvalCondition(cond.substr(p + 2));
    p = FindTopLevel(cond, "&&");
    if (p != std::string::npos)
        return EvalCondition(cond.substr(0, p)) && EvalCondition(cond.substr(p + 2));

    // Single comparison.
    std::string ops[] = {">=", "<=", "!=", "==", ">", "<"};
    std::string op;
    size_t opPos = std::string::npos;
    for (auto& o : ops) {
        opPos = cond.find(o);
        if (opPos != std::string::npos) { op = o; break; }
    }
    if (opPos == std::string::npos) return false;

    std::string lhs = cond.substr(0, opPos);
    std::string rhs = cond.substr(opPos + op.size());

    trim(lhs); trim(rhs);
    while (EnclosesWhole(lhs)) { lhs = lhs.substr(1, lhs.size() - 2); trim(lhs); }
    while (EnclosesWhole(rhs)) { rhs = rhs.substr(1, rhs.size() - 2); trim(rhs); }

    auto resolve = [&](const std::string& s) -> float {
        if (s.rfind("$", 0) == 0) return ResolveVar(s.substr(1));
        try { return std::stof(s); } catch (...) { return 0; }
    };

    float lv = resolve(lhs);
    float rv = resolve(rhs);

    if (op == "==") return lv == rv;
    if (op == "!=") return lv != rv;
    if (op == ">")  return lv > rv;
    if (op == "<")  return lv < rv;
    if (op == ">=") return lv >= rv;
    if (op == "<=") return lv <= rv;
    return false;
}

// ---------------------------------------------------------------------------
// PopPendingSound — return and clear __last_sound
// ---------------------------------------------------------------------------
std::string LightningScriptContext::PopPendingSound() {
    auto it = m_strVars.find("__last_sound");
    if (it == m_strVars.end() || it->second.empty()) return {};
    std::string name = it->second;
    m_strVars.erase(it);
    return name;
}

// ---------------------------------------------------------------------------
// PopPendingFog — read and clear pending fog values
// Returns true if fog values are available.
// ---------------------------------------------------------------------------
bool LightningScriptContext::PopPendingFog(float& r, float& g, float& b, float& density) {
    auto it_r = m_floatVars.find("__fog_r");
    if (it_r == m_floatVars.end()) return false;
    r = it_r->second; m_floatVars.erase(it_r);
    auto it_g = m_floatVars.find("__fog_g");
    auto it_b = m_floatVars.find("__fog_b");
    auto it_d = m_floatVars.find("__fog_density");
    if (it_g != m_floatVars.end()) { g = it_g->second; m_floatVars.erase(it_g); }
    if (it_b != m_floatVars.end()) { b = it_b->second; m_floatVars.erase(it_b); }
    if (it_d != m_floatVars.end()) { density = it_d->second; m_floatVars.erase(it_d); }
    return true;
}

// ---------------------------------------------------------------------------
// PopPendingSkybox — return and clear __skybox
// ---------------------------------------------------------------------------
std::string LightningScriptContext::PopPendingSkybox() {
    auto it = m_strVars.find("__skybox");
    if (it == m_strVars.end()) return {};
    std::string name = it->second;
    m_strVars.erase(it);
    return name;
}

bool LightningScriptContext::PopPendingFogRestore() {
    auto it = m_floatVars.find("__fog_restore");
    if (it == m_floatVars.end()) return false;
    const bool set = it->second != 0.0f;
    m_floatVars.erase(it);
    return set;
}

bool LightningScriptContext::PopPendingAmbientRestore() {
    auto it = m_floatVars.find("__ambient_restore");
    if (it == m_floatVars.end()) return false;
    const bool set = it->second != 0.0f;
    m_floatVars.erase(it);
    return set;
}

bool LightningScriptContext::PopPendingSkyboxRestore() {
    auto it = m_strVars.find("__skybox_restore");
    if (it == m_strVars.end()) return false;
    m_strVars.erase(it);
    return true;
}

bool LightningScriptContext::PopPendingAmbient(float& r, float& g, float& b) {
    auto it_r = m_floatVars.find("__ambient_r");
    if (it_r == m_floatVars.end()) return false;
    r = it_r->second; m_floatVars.erase(it_r);
    auto it_g = m_floatVars.find("__ambient_g");
    auto it_b = m_floatVars.find("__ambient_b");
    if (it_g != m_floatVars.end()) { g = it_g->second; m_floatVars.erase(it_g); }
    if (it_b != m_floatVars.end()) { b = it_b->second; m_floatVars.erase(it_b); }
    return true;
}

LightningScriptContext::PawnSpawnRequest LightningScriptContext::PopPendingPawnSpawn() {
    PawnSpawnRequest req;
    auto it = m_strVars.find("__pawn_name");
    if (it == m_strVars.end() || it->second.empty()) return req;
    req.name = it->second;
    m_strVars.erase(it);
    auto it_x = m_floatVars.find("__pawn_x");
    auto it_y = m_floatVars.find("__pawn_y");
    auto it_z = m_floatVars.find("__pawn_z");
    if (it_x != m_floatVars.end()) { req.x = it_x->second; m_floatVars.erase(it_x); }
    if (it_y != m_floatVars.end()) { req.y = it_y->second; m_floatVars.erase(it_y); }
    if (it_z != m_floatVars.end()) { req.z = it_z->second; m_floatVars.erase(it_z); }
    req.valid = true;
    return req;
}

// ---------------------------------------------------------------------------
// PopPendingMessage — return and clear __last_msg
// ---------------------------------------------------------------------------
std::string LightningScriptContext::PopPendingMessage() {
    auto it = m_strVars.find("__last_msg");
    if (it == m_strVars.end() || it->second.empty()) return {};
    std::string name = it->second;
    m_strVars.erase(it);
    return name;
}

// ---------------------------------------------------------------------------
// Consume / player stat ops / pickup spawn side-effects
// ---------------------------------------------------------------------------
bool LightningScriptContext::ConsumeRequested() const {
    return GetFloat("__consume", 0.0f) > 0.0f;
}

void LightningScriptContext::ClearConsume() {
    SetFloat("__consume", 0.0f);
}

std::vector<LightningScriptContext::PlayerStatOp> LightningScriptContext::PopPlayerStatOps() {
    std::vector<PlayerStatOp> out;
    out.swap(m_pendingStatOps);
    return out;
}

std::vector<LightningScriptContext::ScriptCommand> LightningScriptContext::PopCommands() {
    std::vector<ScriptCommand> out;
    out.swap(m_commands);
    return out;
}

LightningScriptContext::PickupSpawnRequest LightningScriptContext::PopPendingPickupSpawn() {
    PickupSpawnRequest req;
    auto it = m_strVars.find("__pickup_name");
    if (it == m_strVars.end() || it->second.empty()) return req;
    req.name = it->second;
    m_strVars.erase(it);
    auto it_x = m_floatVars.find("__pickup_x");
    auto it_y = m_floatVars.find("__pickup_y");
    auto it_z = m_floatVars.find("__pickup_z");
    auto it_r = m_floatVars.find("__pickup_respawn");
    if (it_x != m_floatVars.end()) { req.x = it_x->second; m_floatVars.erase(it_x); }
    if (it_y != m_floatVars.end()) { req.y = it_y->second; m_floatVars.erase(it_y); }
    if (it_z != m_floatVars.end()) { req.z = it_z->second; m_floatVars.erase(it_z); }
    if (it_r != m_floatVars.end()) { req.respawnTime = it_r->second; m_floatVars.erase(it_r); }
    req.valid = true;
    return req;
}

// ---------------------------------------------------------------------------
// PopPendingHurt — return/clear one scripted damage flash request
// ---------------------------------------------------------------------------
bool LightningScriptContext::PopPendingHurt() {
    auto it = m_floatVars.find("__last_hurt");
    if (it == m_floatVars.end()) return false;
    m_floatVars.erase(it);
    return true;
}
