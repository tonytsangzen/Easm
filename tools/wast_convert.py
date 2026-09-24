#!/usr/bin/env python3
"""wast -> wast2json-compatible JSON converter for files whose syntax the
local wabt cannot parse (GC text format, rec groups, ...).

Uses wasm-tools (parse) for text modules; mirrors wast2json's JSON command
schema so tools/wast.c can consume the output unchanged.

Usage: wast_convert.py <file.wast> -o <out.json> [--wasm-tools PATH]
Exit 0 on success; nonzero with message on stderr if conversion is impossible.
"""
import json
import os
import re
import struct
import subprocess
import sys


# ---------------------------------------------------------------- lexing
def tokenize(text):
    """Yield (kind, value, line) tokens: paren, string, atom."""
    toks = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == '\n':
            line += 1
            i += 1
        elif c in ' \t\r':
            i += 1
        elif text.startswith(';;', i):
            while i < n and text[i] != '\n':
                i += 1
        elif text.startswith('(;', i):
            depth, i = 1, i + 2
            while i < n and depth:
                if text.startswith('(;', i):
                    depth += 1
                    i += 2
                elif text.startswith(';)', i):
                    depth -= 1
                    i += 2
                else:
                    if text[i] == '\n':
                        line += 1
                    i += 1
        elif c == '(' or c == ')':
            toks.append((c, c, line))
            i += 1
        elif c == '"':
            j = i + 1
            buf = bytearray()
            while j < n and text[j] != '"':
                if text[j] == '\\':
                    j += 1
                    esc = text[j]
                    if esc == 'n':
                        buf.append(10)
                    elif esc == 't':
                        buf.append(9)
                    elif esc == 'r':
                        buf.append(13)
                    elif esc == '"':
                        buf.append(34)
                    elif esc == "'":
                        buf.append(39)
                    elif esc == '\\':
                        buf.append(92)
                    elif esc == 'u':
                        j += 1
                        cp = 0
                        while text[j] != '}':
                            cp = cp * 16 + int(text[j], 16)
                            j += 1
                        j += 1
                        buf.extend(chr(cp).encode('utf-8'))
                        j -= 1
                    else:
                        buf.append(int(text[j:j + 2], 16))
                        j += 1
                    j += 1
                else:
                    if text[j] == '\n':
                        line += 1
                    buf.append(ord(text[j]))
                    j += 1
            toks.append(('str', bytes(buf), line))
            i = j + 1
        else:
            j = i
            while j < n and text[j] not in ' \t\r\n()";':
                j += 1
            toks.append(('atom', text[i:j], line))
            i = j
    return toks


def split_commands(toks):
    """Split token list into top-level s-expressions (as token lists)."""
    cmds = []
    i = 0
    while i < len(toks):
        if toks[i][0] != '(':
            i += 1
            continue
        depth = 0
        start = i
        while i < len(toks):
            if toks[i][0] == '(':
                depth += 1
            elif toks[i][0] == ')':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        cmds.append(toks[start:i + 1])
        i += 1
    return cmds


def cmd_head(cmd):
    """(kind, head_atoms_after_open) e.g. ('module', ['binary'])."""
    if len(cmd) < 2 or cmd[0][0] != '(':
        return None, []
    i = 1
    heads = []
    while i < len(cmd) and cmd[i][0] == 'atom':
        heads.append(cmd[i][1])
        i += 1
    return heads[0] if heads else None, heads[1:]


# ---------------------------------------------------------------- values
def parse_int(s):
    s = s.replace('_', '')
    neg = s.startswith('-')
    if neg:
        s = s[1:]
    if s.lower().startswith('0x'):
        v = int(s, 16)
    else:
        v = int(s, 10)
    return -v if neg else v


def u_of(ty, v):
    if ty == 'i32':
        return v & 0xFFFFFFFF
    if ty == 'i64':
        return v & 0xFFFFFFFFFFFFFFFF
    return v


def float_bits(ty, s):
    """Return (value_str) in wast2json convention: decimal bit pattern or
    nan:canonical / nan:arithmetic marker."""
    s = s.replace('_', '')
    if s in ('nan:canonical', 'nan:arithmetic', 'nan'):
        return s if s != 'nan' else 'nan:canonical'
    m = re.match(r'^([+-]?)nan:0x([0-9a-fA-F_]+)$', s)
    if m:
        payload = int(m.group(2).replace('_', ''), 16)
        if ty == 'f32':
            bits = 0x7FC00000 | (payload & 0x3FFFFF)
            if m.group(1) == '-':
                bits |= 0x80000000
            return str(bits)
        bits = 0x7FF8000000000000 | (payload & 0xFFFFFFFFFFFFF)
        if m.group(1) == '-':
            bits |= 0x8000000000000000
        return str(bits)
    if s in ('inf', '+inf'):
        return str(0x7F800000 if ty == 'f32' else 0x7FF0000000000000)
    if s == '-inf':
        return str(0xFF800000 if ty == 'f32' else 0xFFF0000000000000)
    try:
        if s.lower().startswith(('0x', '-0x', '+0x')):
            f = float.fromhex(s)
        else:
            f = float(s)
    except ValueError:
        raise ValueError('bad float %r' % s)
    if ty == 'f32':
        return str(struct.unpack('<I', struct.pack('<f', f))[0])
    return str(struct.unpack('<Q', struct.pack('<d', f))[0])


def val_from_const(toks, i):
    """Parse a const expression starting at '(' index i (toks[i]='(').
    Returns (json_value_dict, next_index)."""
    # (i32.const N) (f32.const N) (ref.null T) (ref.extern N) (ref.i31 N)
    # (ref.func) (v128.const LX ...) (struct.new ...) etc.
    kind, heads = cmd_head(toks[i:])
    if kind in ('i32.const', 'i64.const'):
        v = parse_int(toks[i + 2][1])
        ty = kind.split('.')[0]
        return {'type': ty, 'value': str(u_of(ty, v))}, skip_sexpr(toks, i)
    if kind in ('f32.const', 'f64.const'):
        ty = kind.split('.')[0]
        return {'type': ty, 'value': float_bits(ty, toks[i + 2][1])}, skip_sexpr(toks, i)
    if kind == 'ref.null':
        ht = toks[i + 2][1] if i + 2 < len(toks) and toks[i + 2][0] == 'atom' else 'any'
        ty = ref_json_type(ht)
        return {'type': ty, 'value': 'null'}, skip_sexpr(toks, i)
    if kind == 'ref.extern':
        if i + 2 < len(toks) and toks[i + 2][0] == 'atom':
            return {'type': 'externref', 'value': str(parse_int(toks[i + 2][1]))}, skip_sexpr(toks, i)
        return {'type': 'externref', 'value': 'any'}, skip_sexpr(toks, i)
    if kind == 'ref.host':
        # host references share the externref encoding (value N -> ptr N+1)
        if i + 2 < len(toks) and toks[i + 2][0] == 'atom':
            return {'type': 'externref', 'value': str(parse_int(toks[i + 2][1]))}, skip_sexpr(toks, i)
        return {'type': 'externref', 'value': '0'}, skip_sexpr(toks, i)
    if kind in ('ref.i31', 'ref.func', 'struct.new', 'array.new', 'any.convert_extern',
                'ref.struct', 'ref.array', 'ref.eq', 'ref.i31_shared',
                'struct.new_default', 'array.new_default', 'array.new_data',
                'array.new_elem', 'extern.convert_any'):
        ty = 'funcref' if kind == 'ref.func' else 'anyref'
        return {'type': ty, 'value': '0'}, skip_sexpr(toks, i)
    if kind == 'v128.const':
        lanes = []
        lt = toks[i + 2][1]
        for t in toks[i + 3:]:
            if t[0] == ')':
                break
            if t[0] == 'atom':
                if lt.startswith('f'):
                    lanes.append(float_bits(lt, t[1]))
                else:
                    lanes.append(str(u_of('i64', parse_int(t[1]))))
        j = skip_sexpr(toks, i)
        return {'type': 'v128', 'lane_type': lt[1:] if lt[0] in 'if' and 'x' in lt else lt,
                'lane_values': lanes}, j
    # unknown const-producing expr: treat as opaque non-null ref
    return {'type': 'anyref', 'value': '0'}, skip_sexpr(toks, i)


def ref_json_type(ht):
    return {'func': 'funcref', 'extern': 'externref', 'any': 'anyref',
            'none': 'anyref', 'noextern': 'externref', 'nofunc': 'funcref',
            'eq': 'anyref', 'i31': 'anyref', 'struct': 'anyref',
            'array': 'anyref', 'exn': 'exnref'}.get(ht, 'anyref')


def skip_sexpr(toks, i):
    depth = 0
    while i < len(toks):
        if toks[i][0] == '(':
            depth += 1
        elif toks[i][0] == ')':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


# ---------------------------------------------------------------- actions
def parse_action(toks, i):
    """toks[i] == '(' opening an action: (invoke ...) or (get ...)."""
    kind = toks[i + 1][1]
    act = {'type': kind}
    j = i + 2
    # optional module identifier ($M) precedes the field name
    if toks[j][0] == 'atom' and toks[j][1].startswith('$'):
        j += 1
    if toks[j][0] == 'str':
        field = toks[j][1].decode('latin1')
    elif toks[j][0] == 'atom':
        field = toks[j][1].lstrip('$')
    else:
        field = None
    j += 1
    if kind == 'invoke':
        args = []
        while toks[j][0] != ')':
            if toks[j][0] == '(':
                v, j = val_from_const(toks, j)
                args.append(v)
            else:
                j += 1
        act['field'] = field
        act['args'] = args
        return act, j + 1
    if kind == 'get':
        # skip to the close paren
        while toks[j][0] != ')':
            j += 1
        act['field'] = field
        act['args'] = []
        return act, j + 1
    raise ValueError('unknown action %s' % kind)


# ---------------------------------------------------------------- module
def wat_from_module(cmd, start_idx, name=None):
    """Rebuild module text from token stream (keeps original atoms/strings)."""
    out = ['(module']
    if name:
        out.append(' ' + name)
    line0 = cmd[0][2]
    cur_line = line0
    end = len(cmd) - 1  # exclude exactly the top-level close paren
    for t in cmd[start_idx:end]:
        k, v, ln = t
        out.append('\n' * (ln - cur_line))
        cur_line = ln
        if k == 'str':
            esc = []
            for byte in v:
                ch = chr(byte)
                if ch == '\\':
                    esc.append('\\\\')
                elif ch == '"':
                    esc.append('\\"')
                elif ch == '\n':
                    esc.append('\\n')
                elif ch == '\t':
                    esc.append('\\t')
                elif ch == '\r':
                    esc.append('\\r')
                elif 0x20 <= byte < 0x7F:
                    esc.append(ch)
                else:
                    esc.append('\\%02x' % byte)
            out.append('"' + ''.join(esc) + '"')
        elif k == 'atom':
            out.append(v)
        else:
            out.append(v)
        out.append(' ')
    out.append(')')
    return ''.join(out)


def unescape_wast_string(b):
    """wast string bytes already decoded by tokenizer; return raw bytes."""
    return bytes(b)


def convert(in_path, out_path, wasm_tools):
    src = open(in_path, encoding='utf-8', errors='replace').read()
    toks = tokenize(src)
    cmds = split_commands(toks)
    outdir = os.path.dirname(os.path.abspath(out_path))
    stem = os.path.splitext(os.path.basename(in_path))[0]
    commands = []
    n = 0  # module counter
    for cmd in cmds:
        line = cmd[0][2]
        kind, heads = cmd_head(cmd)
        if kind == 'module' and not heads:
            n += 1
            fname = '%s.%d.wasm' % (stem, n - 1)
            body = wat_from_module(cmd, 2)
            wat = os.path.join(outdir, fname + '.wat')
            wasm = os.path.join(outdir, fname)
            open(wat, 'w').write(body)
            r = subprocess.run([wasm_tools, 'parse', wat, '-o', wasm],
                               capture_output=True, text=True)
            if r.returncode != 0:
                sys.stderr.write('%s: text module failed: %s\n' % (in_path, r.stderr.strip()[:200]))
                return 1
            ent = {'type': 'module', 'line': line, 'filename': fname}
            commands.append(ent)
        elif kind == 'module' and heads and heads[0].startswith('$'):
            # named text module: (module $name ...)
            n += 1
            fname = '%s.%d.wasm' % (stem, n - 1)
            body = wat_from_module(cmd, 2, heads[0])
            wat = os.path.join(outdir, fname + '.wat')
            wasm = os.path.join(outdir, fname)
            open(wat, 'w').write(body)
            r = subprocess.run([wasm_tools, 'parse', wat, '-o', wasm],
                               capture_output=True, text=True)
            if r.returncode != 0:
                sys.stderr.write('%s: text module failed: %s\n' % (in_path, r.stderr.strip()[:200]))
                return 1
            ent = {'type': 'module', 'line': line, 'filename': fname, 'name': heads[0]}
            commands.append(ent)
        elif kind == 'module' and heads and heads[0] == 'binary':
            n += 1
            fname = '%s.%d.wasm' % (stem, n - 1)
            data = b''.join(t[1] for t in cmd if t[0] == 'str')
            open(os.path.join(outdir, fname), 'wb').write(data)
            ent = {'type': 'module', 'line': line, 'filename': fname}
            commands.append(ent)
        elif kind == 'module' and heads and heads[0] == 'quote':
            n += 1
            text = b' '.join(t[1] for t in cmd if t[0] == 'str').decode('latin1')
            fname = '%s.%d.wasm' % (stem, n - 1)
            wat = os.path.join(outdir, fname + '.wat')
            wasm = os.path.join(outdir, fname)
            open(wat, 'w').write('(module %s)' % text)
            r = subprocess.run([wasm_tools, 'parse', wat, '-o', wasm],
                               capture_output=True, text=True)
            ent = {'type': 'module', 'line': line, 'filename': fname}
            commands.append(ent)
        elif kind == 'module' and heads and heads[0] == 'definition':
            n += 1
            name = heads[1] if len(heads) > 1 else None
            fname = '%s.%d.wasm' % (stem, n - 1)
            body = wat_from_module(cmd, 3 if name else 2, name)
            wat = os.path.join(outdir, fname + '.wat')
            wasm = os.path.join(outdir, fname)
            open(wat, 'w').write(body)
            r = subprocess.run([wasm_tools, 'parse', wat, '-o', wasm],
                               capture_output=True, text=True)
            if r.returncode != 0:
                sys.stderr.write('%s: text module failed: %s\n' % (in_path, r.stderr.strip()[:200]))
                return 1
            ent = {'type': 'module', 'line': line, 'filename': fname, 'definition': 'true'}
            if name:
                ent['name'] = name.decode('latin1') if isinstance(name, bytes) else name
            commands.append(ent)
        elif kind == 'module' and heads and heads[0] == 'instance':
            ent = {'type': 'instance', 'line': line,
                   'instance': heads[1] if len(heads) > 1 else None,
                   'definition': heads[2] if len(heads) > 2 else heads[1]}
            commands.append(ent)
        elif kind == 'register':
            as_name = cmd[2][1].decode('latin1') if isinstance(cmd[2][1], bytes) else cmd[2][1]
            ent = {'type': 'register', 'line': line, 'as': as_name}
            if len(cmd) > 3 and cmd[3][0] == 'atom':
                ent['name'] = cmd[3][1]
            commands.append(ent)
        elif kind in ('invoke', 'get'):
            act, _ = parse_action(cmd, 0)
            act['field'] = act['field'].lstrip('$') if act['field'] else act['field']
            commands.append({'type': 'action', 'line': line, 'action': act})
        elif kind in ('assert_return', 'assert_trap', 'assert_exn',
                      'assert_invalid', 'assert_malformed', 'assert_unlinkable',
                      'assert_uninstantiable'):
            j = 1
            # skip module-class atoms (e.g. "module", "binary"/"quote"/"text")
            is_binary_form = False
            while j < len(cmd) and cmd[j][0] == 'atom':
                if cmd[j][1] in ('binary', 'quote', 'text'):
                    is_binary_form = cmd[j][1] in ('binary', 'quote')
                j += 1
            ent = {'type': kind, 'line': line}
            if kind in ('assert_return', 'assert_trap', 'assert_exn'):
                act, j2 = parse_action(cmd, j)
                act['field'] = act['field'].lstrip('$') if act['field'] else act['field']
                ent['action'] = act
                j = j2
                expected = []
                while j < len(cmd) and cmd[j][0] == '(':
                    k2, h2 = cmd_head(cmd[j:])
                    if k2 and (k2.endswith('.const') or k2.startswith('ref.') or
                               k2 == 'v128.const'):
                        v, j = val_from_const(cmd, j)
                        expected.append(v)
                    else:
                        j = skip_sexpr(cmd, j)
                ent['expected'] = expected
                if kind in ('assert_trap', 'assert_exn'):
                    # the expected message is the LAST string in the command
                    texts = [t[1].decode('latin1') for t in cmd if t[0] == 'str']
                    ent['text'] = texts[-1] if texts else ''
            elif kind in ('assert_invalid', 'assert_malformed', 'assert_unlinkable',
                          'assert_uninstantiable'):
                module_cmd = cmd[j] if j < len(cmd) and cmd[j][0] == '(' else None
                mk, mh = cmd_head(module_cmd) if module_cmd else (None, [])
                ent['text'] = ''
                strs = [t[1].decode('latin1') for t in cmd if t[0] == 'str']
                if strs:
                    ent['text'] = strs[-1]
                if mk == 'module' and (not mh or mh[0] != 'binary'):
                    n += 1
                    fname = '%s.%d.wasm' % (stem, n - 1)
                    body = wat_from_module(module_cmd, 1)
                    wat = os.path.join(outdir, fname + '.wat')
                    wasm = os.path.join(outdir, fname)
                    open(wat, 'w').write(body)
                    r = subprocess.run([wasm_tools, 'parse', wat, '-o', wasm],
                                       capture_output=True, text=True)
                    if r.returncode != 0:
                        # grammar-level malformed: emit as text so runner skips
                        os.remove(wat)
                        n -= 1
                        ent['module_type'] = 'text'
                        commands.append(ent)
                        continue
                    ent['filename'] = fname
                    ent['module_type'] = 'binary'
                elif mk == 'module':
                    n += 1
                    fname = '%s.%d.wasm' % (stem, n - 1)
                    data = b''.join(t[1] for t in module_cmd if t[0] == 'str')
                    open(os.path.join(outdir, fname), 'wb').write(data)
                    ent['filename'] = fname
                    ent['module_type'] = 'binary'
                else:
                    continue
            commands.append(ent)
        else:
            continue
    # drop None instance names (json)
    for ent in commands:
        if ent.get('type') == 'instance' and ent.get('instance') is None:
            del ent['instance']
    json.dump({'source_filename': in_path, 'commands': commands},
              open(out_path, 'w'), indent=1)
    return 0


if __name__ == '__main__':
    args = sys.argv[1:]
    wt = '/tmp/wasm-tools-1.259.0-aarch64-macos/wasm-tools'
    if not os.path.exists(wt):  # /tmp is volatile; fall back to PATH
        import shutil
        found = shutil.which('wasm-tools')
        if found:
            wt = found
    if '--wasm-tools' in args:
        i = args.index('--wasm-tools')
        wt = args[i + 1]
        args = args[:i] + args[i + 2:]
    inf = args[0]
    out = args[args.index('-o') + 1] if '-o' in args else os.path.splitext(inf)[0] + '.json'
    sys.exit(convert(inf, out, wt))
