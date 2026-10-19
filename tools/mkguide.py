#!/usr/bin/env python3
"""mkguide.py [docs/manual] [PrismNet.guide] - the user manual as an AmigaGuide document.

The manual's Markdown (docs/manual/*.md, read in name order; README.md is the
contents page) is the one source: GitHub shows it as it is, and this script
turns it into the .guide that ships in the package. Each chapter is a node
with buttons for its sections; each "##" section is a node of its own.

Handled: # ## ### headings, paragraphs, "-" and "1." lists, ``` blocks,
tables, **bold**, *italic*, `code`, and links - to another chapter or
section they become buttons, to the web they become "text (url)".
Text is wrapped at 76 columns here (no @wordwrap), so it reads the same with
every amigaguide.library from OS 3.0 on. GPL-3.0-only, see LICENSE.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'docs', 'manual')
OUT = sys.argv[2] if len(sys.argv) > 2 else 'PrismNet.guide'
WIDTH = 76

# inline markers, replaced by AmigaGuide commands at output time
B_ON, B_OFF, I_ON, I_OFF, L_ON, L_MID, L_OFF, NBSP = '\x01', '\x02', '\x03', '\x04', '\x05', '\x06', '\x07', '\x08'


def slug(text):
    """GitHub's heading anchor"""
    t = re.sub(r'[`*_]', '', text).strip().lower()
    t = re.sub(r'[^\w\- ]', '', t)
    return t.replace(' ', '-')


def esc(s):
    return s.replace('\\', '\\\\').replace('@', '\\@')


def visible(s):
    return len(re.sub(L_ON + '|' + L_MID + '[^' + L_OFF + ']*' + L_OFF + '|[\x01-\x04]', '', s).replace(NBSP, ' ')) + \
        2 * s.count(L_ON)                       # a button shows " label "


def render(s):
    s = s.replace(B_ON, '@{b}').replace(B_OFF, '@{ub}').replace(I_ON, '@{i}').replace(I_OFF, '@{ui}')
    s = re.sub(L_ON + '([^' + L_MID + ']*)' + L_MID + '([^' + L_OFF + ']*)' + L_OFF,
               lambda m: '@{" %s " link %s}' % (m.group(1), m.group(2)), s)
    return s.replace(NBSP, ' ')


class Manual:
    def __init__(self, src):
        self.files = sorted(f for f in os.listdir(src) if f.endswith('.md') and f != 'README.md')
        self.src = src
        self.chapters = []          # (file, node, title, intro blocks, [(node, title, blocks)])
        self.anchors = {}           # (file, anchor) -> node
        for f in ['README.md'] + self.files:
            self.parse(f)

    def parse(self, f):
        lines = open(os.path.join(self.src, f), encoding='utf-8').read().replace('\r', '').split('\n')
        base = 'Main' if f == 'README.md' else re.sub(r'^\d+-', '', f[:-3])
        title, intro, sections, cur = base, [], [], None
        for l in lines:
            m = re.match(r'^(#{1,2}) (.*)', l)
            if m and not (cur and cur[3]):                      # not inside a ``` block
                if m.group(1) == '##' and f == 'README.md':     # the contents page: one node
                    intro.append('### ' + m.group(2))
                    continue
                if m.group(1) == '#':
                    title = m.group(2).strip()
                    self.anchors[(f, '')] = base
                    self.anchors[(f, slug(title))] = base
                    continue
                node = base + '-' + slug(m.group(2))[:24].strip('-')
                cur = [node, m.group(2).strip(), [], False]
                sections.append(cur)
                self.anchors[(f, slug(m.group(2)))] = node
                continue
            target = cur[2] if cur else intro
            target.append(l)
            if l.startswith('```') and cur:
                cur[3] = not cur[3]
        self.anchors.setdefault((f, ''), base)
        self.chapters.append((f, base, title, intro, [(n, t, b) for n, t, b, _ in sections]))

    # ---- inline text -----------------------------------------------------------
    def inline(self, text, f):
        out, pos = [], 0
        pat = re.compile(r'\[([^\]]+)\]\(([^)]+)\)|\*\*(.+?)\*\*|(?<![\w*])\*([^*\s][^*]*?)\*(?!\w)|`([^`]+)`')
        for m in pat.finditer(text):
            out.append(esc(text[pos:m.start()]))
            pos = m.end()
            if m.group(1) is not None:
                out.append(self.link(m.group(1), m.group(2), f))
            elif m.group(3) is not None:
                out.append(B_ON + self.inline(m.group(3), f) + B_OFF)
            elif m.group(4) is not None:
                out.append(I_ON + self.inline(m.group(4), f) + I_OFF)
            else:
                out.append(esc(m.group(5)))
        out.append(esc(text[pos:]))
        return ''.join(out)

    def link(self, label, url, f):
        plain = esc(re.sub(r'[`*]', '', label))
        if re.match(r'^[a-z]+://', url):
            return plain + ' (' + esc(url) + ')'
        file, _, anchor = url.partition('#')
        file = os.path.basename(file) if file else f
        node = self.anchors.get((file, anchor))
        if not node:
            return plain                     # outside the manual (a design doc, the source)
        return L_ON + plain.replace(' ', NBSP) + L_MID + node + L_OFF

    # ---- blocks ------------------------------------------------------------------
    def wrap(self, text, first, rest):
        words, lines, line, n = text.split(), [], first, len(first)
        for w in words:
            wl = visible(w)
            if n > len(first if not lines else rest) and n + 1 + wl > WIDTH:
                lines.append(line)
                line, n = rest + w, len(rest) + wl
            else:
                if n > len(first if not lines else rest):
                    line += ' '; n += 1
                line += w; n += wl
        lines.append(line)
        return [render(l) for l in lines]

    def table(self, rows, f):
        cells = [[re.sub(r'[`*]', '', c.strip()) for c in r.strip().strip('|').split('|')] for r in rows]
        cells = [r for r in cells if not all(re.match(r'^:?-+:?$', c) for c in r)]
        cells = [[re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', c) for c in r] for r in cells]
        ncol = max(len(r) for r in cells)
        cells = [r + [''] * (ncol - len(r)) for r in cells]
        widths = [max(len(r[i]) for r in cells) for i in range(ncol)]
        out = []
        if sum(widths) + 2 * (ncol - 1) + 2 <= WIDTH:
            for k, r in enumerate(cells):
                line = '  ' + '  '.join(c.ljust(widths[i]) for i, c in enumerate(r))
                out.append(esc(line.rstrip()))
                if k == 0:
                    out.append('  ' + '  '.join('-' * w for w in widths))
            return out
        head = cells[0]
        for r in cells[1:]:                  # too wide: one block a row
            out.append('  @{b}' + esc(r[0]) + '@{ub}')
            for i in range(1, ncol):
                if r[i] and ncol == 2:       # term and description: no label needed
                    out += self.wrap(esc(r[i]), '      ', '      ')
                elif r[i]:
                    lead = '      ' + (head[i] + ': ' if head[i] else '')
                    out += self.wrap(esc(r[i]), lead, '        ')
        return out

    def blocks(self, lines, f):
        out, i = [], 0
        while i < len(lines):
            l = lines[i]
            if not l.strip():
                if out and out[-1] != '':
                    out.append('')
                i += 1
            elif l.startswith('```'):
                i += 1
                while i < len(lines) and not lines[i].startswith('```'):
                    out.append('    ' + esc(lines[i]))
                    i += 1
                i += 1
            elif l.startswith('|'):
                rows = []
                while i < len(lines) and lines[i].startswith('|'):
                    rows.append(lines[i]); i += 1
                out += self.table(rows, f)
            elif l.startswith('### '):
                out.append('@{b}' + render(self.inline(l[4:].strip(), f)) + '@{ub}')
                i += 1
            elif re.match(r'^\s*(- |\d+\. )', l):
                m = re.match(r'^(\s*)(- |\d+\. )(.*)', l)
                depth = len(m.group(1)) // 2
                text = m.group(3)
                i += 1
                while i < len(lines) and lines[i].strip() and not re.match(r'^\s*(- |\d+\. )', lines[i]) \
                        and lines[i].startswith(' '):
                    text += ' ' + lines[i].strip(); i += 1
                bullet = '*' if m.group(2) == '- ' else m.group(2).strip()
                pad = '  ' + '  ' * depth
                out += self.wrap(self.inline(text, f), pad + bullet + ' ', pad + ' ' * (len(bullet) + 1))
            else:
                text = l.strip()
                i += 1
                while i < len(lines) and lines[i].strip() and not re.match(r'^(\s*(- |\d+\. )|```|\||#)', lines[i]):
                    text += ' ' + lines[i].strip(); i += 1
                out += self.wrap(self.inline(text, f), '', '')
        while out and out[-1] == '':
            out.pop()
        return out

    # ---- output --------------------------------------------------------------------
    def write(self, out, version):
        order = []
        for f, base, title, intro, sections in self.chapters:
            order.append(base)
            order += [n for n, _, _ in sections]
        nxt = {a: b for a, b in zip(order, order[1:])}
        prv = {b: a for a, b in zip(order, order[1:])}

        def head(node, title, toc):
            s = '@node %s "%s"\n' % (node, esc(title))
            if toc: s += '@toc %s\n' % toc
            if node in prv: s += '@prev %s\n' % prv[node]
            if node in nxt: s += '@next %s\n' % nxt[node]
            return s

        g = ['@database PrismNet.guide',
             '@$VER: PrismNet.guide %s' % version,
             '@author "Laine Jones"',
             '@(c) "GPL-3.0-only"', '']
        for f, base, title, intro, sections in self.chapters:
            g.append(head(base, title, None if base == 'Main' else 'Main').rstrip('\n'))
            g.append('@{b}' + esc(title) + '@{ub}')
            g.append('')
            g += self.blocks(intro, f)
            if sections:
                g.append('')
                for n, t, _ in sections:
                    g.append('  @{" %s " link %s}' % (esc(re.sub(r'[`*]', '', t)), n))
            g += ['@endnode', '']
            for n, t, b in sections:
                g.append(head(n, re.sub(r'[`*]', '', t), base).rstrip('\n'))
                g.append('@{b}' + esc(re.sub(r'[`*]', '', t)) + '@{ub}')
                g.append('')
                g += self.blocks(b, f)
                g += ['@endnode', '']
        data = '\n'.join(g) + '\n'
        def shown(l):
            l = re.sub(r'@\{" (.*?) " link [^}]*\}', r' \1 ', l)
            return len(re.sub(r'@\{u?[bi]\}', '', l).replace('\\@', '@').replace('\\\\', '\\'))
        bad = [l for l in data.split('\n') if shown(l) > 79 and not l.startswith('@')]
        open(out, 'w', encoding='latin-1', errors='replace', newline='\n').write(data)
        return len(order), bad


if __name__ == '__main__':
    ver = os.environ.get('PN_GUIDEVER', '(dev)')     # e.g. "0.6 (18.10.2026)"
    m = Manual(SRC)
    n, bad = m.write(OUT, ver)
    print('mkguide: %d nodes -> %s' % (n, OUT))
    for l in bad[:10]:
        print('  long line:', l[:60])
