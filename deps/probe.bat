@echo off
where perl 2>nul && (perl -e "print qq{PERL_OK $]\n}") || echo NO_PERL
where nasm 2>nul || echo NO_NASM
where nmake 2>nul || echo NO_NMAKE_in_PATH
