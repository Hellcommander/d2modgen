#!/usr/bin/env python3
# Copyright (C) 2026 Smirnov Vladimir / mapron1@gmail.com
# SPDX-License-Identifier: BSL-1.0
# See LICENSE file for details.
"""Writes SettingsHelp.hpp: the caption and tooltip of every setting, taken from qml/Modules/*.qml.

The console app's agent commands (`modgen_console describe`) show this text next to each setting's
type and range, so tools can work out what a setting does without the UI. Run it from this folder
after changing captions or tooltips in qml/Modules (CMake does it with -DENABLE_SCHEMA_GEN=ON).
"""
import html
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
QML = HERE.parent.parent / 'qml' / 'Modules'
OUT = HERE / 'SettingsHelp.hpp'

STRING = r'(?:qsTr\(\s*)?"((?:[^"\\]|\\.)*)"\s*\)?'


def text(raw):
    raw = raw.encode('utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')
    raw = re.sub(r'<br\s*/?>', '\n', raw)
    raw = re.sub(r'<[^>]+>', '', raw)
    return html.unescape(raw).strip()


def blocks(source):
    """Yields (type name, body text without nested blocks, nested blocks) for every `Type { ... }`."""
    out = []
    i = 0
    pattern = re.compile(r'([A-Z][A-Za-z0-9_]*)\s*\{')
    while True:
        m = pattern.search(source, i)
        if not m:
            return out
        depth, j = 1, m.end()
        in_string = False
        while j < len(source) and depth:
            c = source[j]
            if in_string:
                if c == '\\':
                    j += 1
                elif c == '"':
                    in_string = False
            elif c == '"':
                in_string = True
            elif c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
            j += 1
        body = source[m.end():j - 1]
        out.append((m.group(1), body))
        i = j


def own_props(body):
    """Properties written directly in this block (nested blocks removed)."""
    flat, depth, in_string = [], 0, False
    for k, c in enumerate(body):
        if in_string:
            if c == '"' and body[k - 1] != '\\':
                in_string = False
            if depth == 0:
                flat.append(c)
            continue
        if c == '"':
            in_string = True
            if depth == 0:
                flat.append(c)
            continue
        if c == '{':
            depth += 1
            continue
        if c == '}':
            depth -= 1
            continue
        if depth == 0:
            flat.append(c)
    flat = ''.join(flat)
    props = {}
    for name in ('caption', 'tooltip', 'text'):
        m = re.search(r'\b%s\s*:\s*%s' % (name, STRING), flat)
        if m:
            props[name] = text(m.group(1))
    m = re.search(r'\bmodel\s*:\s*\[(.*?)\]', flat, re.S)
    if m:
        props['choices'] = [text(s) for s in re.findall(STRING, m.group(1))]
    for name in ("value_key", "value_key1", "value_key2", "enabled_key", "value_context"):
        m = re.search(r'\b%s\s*:\s*([A-Za-z0-9_.]+)' % name, flat)
        if m:
            props[name] = m.group(1)
    return props


def walk(source, context, title, entries, components):
    label = ''  # the last Label at this level: the caption of a RadioGroup/ComboBox without its own
    for type_name, body in blocks(source):
        props = own_props(body)
        if type_name.startswith('Label') and 'text' in props:
            label = props['text'].rstrip(':')
        ctx = context
        if 'value_context' in props and props['value_context'].startswith('config.'):
            ctx = props['value_context'][len('config.'):]
        if type_name in components and 'value_context' in props:
            # a reusable module (ModuleBalanceItemsCommon) shown for one config section
            prefix = props.get('caption', '')
            walk(components[type_name], ctx, prefix, entries, components)
            entries.setdefault(ctx, (props.get('caption', ''), ''))
            if re.search(r'value_context\.doOverride', components[type_name]):
                entries.setdefault(ctx + '.doOverride', (
                    (prefix + ': ' if prefix else '') + 'Override',
                    'On: this group uses its own values. Off: it uses the Common settings group.'))
            continue
        keys = [('', props.get('value_key'))]
        if props.get('value_key1') or props.get('value_key2'):  # WidgetRangeSlider: min and max
            keys = [(' (minimum)', props.get('value_key1')), (' (maximum)', props.get('value_key2'))]
        for suffix, key in keys:
            if not key:
                continue
            if key.startswith('root.value_context.'):
                path = ctx + '.' + key[len('root.value_context.'):]
            elif key.startswith('config.'):
                path = key[len('config.'):]
            else:
                continue
            caption = props.get('caption', '') or label
            if title and caption:
                caption = title + ': ' + caption
            if caption:
                caption += suffix
            tip = props.get('tooltip', '')
            if props.get('choices'):
                options = 'Values: ' + ', '.join('%d = %s' % (n, c) for n, c in enumerate(props['choices']))
                tip = (tip + '\n' + options) if tip else options
            entries.setdefault(path, (caption, tip))
        walk(body, ctx, title, entries, components)


def main():
    files = sorted(QML.glob('*.qml'))
    components = {f.stem: f.read_text(encoding='utf-8') for f in files}
    reusable = {name for name, src in components.items() if re.search(r'property\s+\w+\s+value_context\s*:\s*null', src)}
    entries = {}
    for f in files:
        if f.stem in reusable:
            continue
        source = components[f.stem]
        top = blocks(source)
        if not top:
            continue
        type_name, body = top[0]
        props = own_props(body)
        ctx = props.get('value_context', '')
        if not ctx.startswith('config.'):
            continue
        ctx = ctx[len('config.'):]
        entries.setdefault(ctx, (props.get('caption', ''), props.get('tooltip', '')))
        enabled = props.get('enabled_key', '')
        for prefix in ('root.value_context.', 'config.' + ctx + '.'):
            if enabled.startswith(prefix):
                entries.setdefault(ctx + '.' + enabled[len(prefix):],
                                   ('Enable: ' + props.get('caption', ''), 'Turns the whole "%s" tab on.' % props.get('caption', '')))
        walk(body, ctx, '', entries, {k: v for k, v in components.items() if k in reusable})

    entries.setdefault('globalSeed', ('', ''))
    for path, (caption, tip) in list(entries.items()):
        if (path.endswith('.seed') or path == 'globalSeed') and not caption:
            entries[path] = ('Random seed', 'Same seed and same settings give the same result. 0 picks a new seed on the next generate.')

    def cpp(s):
        return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'

    lines = [
        '#pragma once',
        '// Do not edit, this file was generated from qml/Modules/*.qml using update_help.py',
        '',
        '#include <string_view>',
        '',
        'namespace D2ModGen {',
        '',
        'struct SettingHelp {',
        '    std::string_view path;    // section ("balanceMonsters") or setting ("balanceMonsters.monsterAttack")',
        '    std::string_view caption; // the UI label',
        '    std::string_view help;    // the UI tooltip, plain text',
        '};',
        '',
        'inline constexpr SettingHelp s_settingsHelp[] = {',
    ]
    for path in sorted(entries):
        caption, tip = entries[path]
        lines.append('    { %s, %s, %s },' % (cpp(path), cpp(caption), cpp(tip)))
    lines += ['};', '', '}', '']
    OUT.write_text('\n'.join(lines), encoding='utf-8', newline='\n')
    print('%s: %d entries' % (OUT.name, len(entries)))


if __name__ == '__main__':
    main()
