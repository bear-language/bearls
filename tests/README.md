## Fuzzing with half-typed input

```bash
python3 tests/prefix_fuzz.py ../bear-lang/tests --step 5 --out fuzz-failures
# hang  .../tests/hir/63.br @ 110 chars: bearc also hangs in the parser (repro: fuzz-failures/hir_63_prefix110.br)
bearc fuzz-failures/hir_63_prefix110.br -I ../bear-lang/tests/hir    # reproduce directly
```

### Test
```bash
python3 tests/lsp_test.py                  # uses build/bearls
python3 tests/lsp_test.py build-debug/bearls
```
