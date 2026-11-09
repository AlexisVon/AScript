#!/usr/bin/env python3
"""Convert lang.md into the block JSON consumed by core::langref().

Standalone (pretty output, for review):
    python3 tools/md2js.py res/core/lang.md            # to stdout
    python3 tools/md2js.py res/core/lang.md out.json   # to a file

From gen_res.py (compact output, embedded into res.cpp):
    import md2js
    text = md2js.to_json(source, compact=True, source=path)

The converter is strict on purpose: lang.md is our own authored document, so a
construct outside the supported set fails generation instead of degrading at run time.
Supported: ATX headings (#/##/###), paragraphs, fenced code, pipe tables with
alignment, ordered/unordered lists, blockquotes, chapter-separator rules, and the
page-break div. Inline markup is limited to `code` and **bold** (bold may contain
code); italic, links, images and raw HTML are rejected.
"""
import json
import re
import sys

DIV = '<div style="page-break-after: always;"></div>'
HEADING = re.compile(r'^(#{1,6})\s+(.*)$')
HR = re.compile(r'^---+\s*$')
BULLET = re.compile(r'^[-*+]\s+(.*)$')
NUMBERED = re.compile(r'^\d+\.\s+(.*)$')
DELIM_CELL = re.compile(r'^:?-+:?$')


class LangError(Exception):
    pass


class _Parser:
    def __init__(self, text, source):
        self.lines = text.split('\n')
        self.source = source
        self.blocks = []
        self.in_chapter = False
        self.has_section = False

    def fail(self, idx, msg):
        raise LangError(f'{self.source}:{idx + 1}: {msg}')

    def check_inline(self, text, idx):
        """Validate the inline grammar; the text itself is kept verbatim."""
        i, n = 0, len(text)
        while i < n:
            c = text[i]
            if c == '`':
                j = text.find('`', i + 1)
                if j < 0:
                    self.fail(idx, 'unclosed code span')
                i = j + 1
                continue
            if text.startswith('**', i):
                j = text.find('**', i + 2)
                if j < 0:
                    self.fail(idx, 'unclosed bold span')
                self.check_inline(text[i + 2:j], idx)
                i = j + 2
                continue
            if c == '*':
                self.fail(idx, "italic or literal '*' is not supported")
            if c == '[' and re.match(r'!?\[[^\]]*\]\([^)]*\)', text[i:]):
                self.fail(idx, 'links and images are not supported')
            if c == '<':
                self.fail(idx, 'raw HTML is not supported')
            i += 1

    def split_row(self, line, idx):
        s = line.strip()
        if not s.startswith('|') or not s.endswith('|'):
            self.fail(idx, 'table row must start and end with |')
        cells, cur, i = [], [], 1
        while i < len(s):
            c = s[i]
            if c == '\\' and i + 1 < len(s) and s[i + 1] == '|':
                cur.append('|')
                i += 2
                continue
            if c == '|':
                cells.append(''.join(cur).strip())
                cur = []
                i += 1
                continue
            cur.append(c)
            i += 1
        if ''.join(cur).strip():
            self.fail(idx, 'table row has trailing content after the last |')
        return cells

    def parse_table(self, start):
        raw = []
        i = start
        while i < len(self.lines) and self.lines[i].strip().startswith('|'):
            raw.append(i)
            i += 1
        if len(raw) < 2:
            self.fail(start, 'table needs a header and a delimiter row')

        head = self.split_row(self.lines[raw[0]], raw[0])
        delim = self.split_row(self.lines[raw[1]], raw[1])
        if len(delim) != len(head):
            self.fail(raw[1], 'delimiter row does not match the header width')
        align = []
        for cell in delim:
            if not DELIM_CELL.match(cell):
                self.fail(raw[1], f'bad delimiter cell: {cell!r}')
            left, right = cell.startswith(':'), cell.endswith(':')
            align.append('center' if left and right else 'right' if right else 'left')

        rows = [head]
        for idx in raw[2:]:
            cells = self.split_row(self.lines[idx], idx)
            if len(cells) != len(head):
                self.fail(idx, 'table row does not match the header width')
            rows.append(cells)

        block = {'type': 'table', 'rows': rows}
        if any(a != 'left' for a in align):
            block['align'] = align
        self.blocks.append(block)
        return i

    def is_block_start(self, s):
        return (not s or HEADING.match(s) or s.startswith(('|', '>', '```', '<')) or
                HR.match(s) or s == DIV or BULLET.match(s) or NUMBERED.match(s))

    def parse(self):
        lines = self.lines
        first = next((k for k, line in enumerate(lines) if line.strip()), None)
        m = HEADING.match(lines[first].strip()) if first is not None else None
        if not m or len(m.group(1)) != 1:
            self.fail(first if first is not None else 0,
                      'document must start with a level-1 heading')
        i = 0
        saw_heading = False
        while i < len(lines):
            line = lines[i]
            s = line.strip()

            if not s:
                i += 1
                continue

            if s.startswith('```'):
                lang = s[3:].strip()
                body = []
                i += 1
                while i < len(lines) and not lines[i].strip().startswith('```'):
                    body.append(lines[i])
                    i += 1
                if i >= len(lines):
                    self.fail(len(lines) - 1, 'unterminated code fence')
                block = {'type': 'code', 'lines': body}
                if lang:
                    block['lang'] = lang
                self.blocks.append(block)
                i += 1
                continue

            if s == DIV:
                i += 1
                continue

            if HR.match(s):
                j = i + 1
                while j < len(lines) and not lines[j].strip():
                    j += 1
                m = HEADING.match(lines[j].strip()) if j < len(lines) else None
                if not m or len(m.group(1)) != 1:
                    self.fail(i, 'rule is only allowed as a chapter separator')
                i += 1
                continue

            m = HEADING.match(s)
            if m:
                level = len(m.group(1))
                text = m.group(2).strip()
                if level > 3:
                    self.fail(i, 'headings deeper than ### are not supported')
                if not text:
                    self.fail(i, 'empty heading')
                self.check_inline(text, i)
                if level == 1:
                    self.in_chapter = True
                    self.has_section = False
                elif not self.in_chapter:
                    self.fail(i, 'section heading before the first chapter')
                elif level == 2:
                    self.has_section = True
                elif not self.has_section:
                    self.fail(i, '### heading without a preceding ##')
                saw_heading = True
                self.blocks.append({'type': 'heading', 'level': level, 'text': text})
                i += 1
                continue

            if s.startswith('|'):
                i = self.parse_table(i)
                continue

            if BULLET.match(s) or NUMBERED.match(s):
                ordered = bool(NUMBERED.match(s))
                items = []
                while i < len(lines):
                    cur = lines[i].strip()
                    m2 = NUMBERED.match(cur) if ordered else BULLET.match(cur)
                    if not m2:
                        break
                    self.check_inline(m2.group(1), i)
                    items.append(m2.group(1))
                    i += 1
                if ordered:
                    self.blocks.append({'type': 'list', 'ordered': True, 'items': items})
                else:
                    self.blocks.append({'type': 'list', 'items': items})
                if i < len(lines) and lines[i].strip():
                    self.fail(i, 'list continuation is not supported')
                continue

            if s.startswith('>'):
                text = s[1:].strip()
                if text:
                    self.check_inline(text, i)
                    self.blocks.append({'type': 'quote', 'text': text})
                i += 1
                if i < len(lines) and not self.is_block_start(lines[i].strip()):
                    self.fail(i, 'blockquote continuation is not supported')
                continue

            if s.startswith('<'):
                self.fail(i, 'raw HTML is not supported')

            para = [s]
            start = i
            i += 1
            while i < len(lines) and not self.is_block_start(lines[i].strip()):
                para.append(lines[i].strip())
                i += 1
            text = ' '.join(para)
            self.check_inline(text, start)
            self.blocks.append({'type': 'paragraph', 'text': text})

        if not saw_heading:
            self.fail(0, 'no headings found')
        return {'blocks': self.blocks}


def convert(text, source='<md>'):
    return _Parser(text, source).parse()


def to_json(text, compact=True, source='<md>'):
    doc = convert(text, source)
    if compact:
        return json.dumps(doc, ensure_ascii=False, separators=(',', ':'))
    return json.dumps(doc, ensure_ascii=False, indent=2) + '\n'


def main():
    if len(sys.argv) not in (2, 3):
        print(f'Usage: {sys.argv[0]} <lang.md> [lang.json]', file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[1], 'r', encoding='utf-8') as f:
        source = f.read()

    try:
        out = to_json(source, compact=False, source=sys.argv[1])
    except LangError as e:
        print(e, file=sys.stderr)
        sys.exit(1)

    if len(sys.argv) == 3:
        with open(sys.argv[2], 'w', encoding='utf-8') as f:
            f.write(out)
        print(f'Generated {sys.argv[2]}')
    else:
        sys.stdout.write(out)


if __name__ == '__main__':
    main()
