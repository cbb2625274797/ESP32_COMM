path = r'tools\style.css'
with open(path, 'r', encoding='utf-8-sig') as f:
    css = f.read()

# 在 .svi-val.err 之后追加 stage-result 样式
old = """.svi-val.err { color: var(--red); font-weight: 600; }"""
new = """.svi-val.err { color: var(--red); font-weight: 600; }

/* Stage result panel - 上位机确认 0x07/0x85 收尾 */
.stage-result {
  margin-top: 12px; padding: 14px 16px;
  border-radius: var(--radius-sm);
  background: var(--bg-elevated); border: 1px solid var(--border);
  display: flex; flex-direction: column; gap: 10px;
}
.stage-result.ok { border-color: rgba(34,197,94,.45); background: rgba(34,197,94,.08); }
.stage-result.err { border-color: rgba(239,68,68,.45); background: rgba(239,68,68,.08); }
.stage-result .sr-head { display: flex; align-items: center; gap: 10px; }
.stage-result .sr-ico {
  width: 32px; height: 32px; border-radius: 50%;
  display: inline-flex; align-items: center; justify-content: center;
  font-size: 18px; font-weight: 700; color: #07121c;
}
.stage-result.ok .sr-ico { background: var(--green); }
.stage-result.err .sr-ico { background: var(--red); }
.stage-result .sr-title { font-size: 16px; font-weight: 700; }
.stage-result.ok .sr-title { color: var(--green); }
.stage-result.err .sr-title { color: var(--red); }
.stage-result .sr-detail { font-size: 13px; color: var(--text); line-height: 1.5; }
.stage-result .sr-actions { display: flex; justify-content: flex-end; gap: 8px; }"""

assert old in css, 'svi-val.err CSS not found'
css = css.replace(old, new, 1)

with open(path, 'w', encoding='utf-8-sig') as f:
    f.write(css)
print('style.css updated')
