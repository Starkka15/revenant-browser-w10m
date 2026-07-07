@echo off
where python 2>nul && (python --version) || echo NO_PYTHON
where py 2>nul && (py --version) || echo NO_PY_LAUNCHER
where meson 2>nul && (meson --version) || echo NO_MESON
where ninja 2>nul && (ninja --version) || echo NO_NINJA
where pip 2>nul || echo NO_PIP
