# HugEngine 项目规则

## Git 规则
- 不自动执行 `git commit` 或 `git push`。需要提交时先询问用户确认。
- 获得提交许可后，默认**按逻辑分开提交**，不要合并成一条。每条提交只做一件事，便于单独
  回退；提交信息与分组直接给出，无需再逐条征求确认。
- Commit log 使用中文。
- Commit log 不得包含 AI 相关信息（如 Co-Authored-By、Generated with Claude Code 等）。
- Commit log 不得包含无效信息（如 `* @` 等格式字符）。

## 代码规范
- 添加代码时必须附带对应的中文注释。
