# Known issues

## Unicode punctuation renders as mojibake in the current Windows alpha

The Windows alpha artifact built from commit `a45e963` may show sequences such
as `â€”`, `Ã—`, or `Â·` where the interface intends to display an em dash,
multiplication sign, or middle dot. This is a presentation defect: it does not
alter peer identities, monitor dimensions, transport data, or saved roaming
configuration.

The source now forces UTF-8 source and execution character sets for MSVC builds.
Install a Windows artifact containing that fix when one is published. The
affected artifact has no runtime setting that can reliably correct the text.
