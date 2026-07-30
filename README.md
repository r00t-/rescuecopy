# rescuecopy

A tool for recovering data from failing storage media. It copies as much readable data as possible while avoiding bad areas, then progressively retries harder-to-read regions.

## Overview

When a storage device is failing, traditional copy tools like `cp` or `dd` will abort on the first error, even advanced tools like `dd_rescue` can stall for long periods on unreadable regions, and offer no way to resume after interruption.
`rescuecopy` addresses this by:

- **Copying good data first** — large readable regions are copied quickly using big buffer reads, while bad areas are skipped and revisited later.
- **Tracking progress persistently** — a metadata file records the status of every sector (not yet copied, successfully copied, or number of failed attempts), so the process can be interrupted and resumed at any time.
- **Protecting progress information** — metadata updates are crash-safe, and a backup copy of the metadata file is maintained that only updates after successful reads, guarding against driver-level failures that cause all reads to fail.
- **Progressively narrowing in on bad regions** — after the initial fast pass, the tool splits remaining uncopied ranges and approaches bad areas from both directions, then finally retries individual failed sectors up to a configurable limit.

The recovery strategy and other details are described in detail in `spec.txt`,
from which most of the code was generated using an LLM.

## Building

```sh
make
```

## Usage

```
rescuecopy [options] input_file output_file
```

### Options

| Option | Description |
|--------|-------------|
| `-z` | Initialize output file with zeroes instead of creating a sparse file |
| `-R` | Reset retry counts in metadata file (marks all failed sectors as uncopied) |
| `-d` | Enable live status display showing sector map and read position |
| `-r <count>` | Max retries per sector |
| `-e <count>` | Max total read errors per run |
| `-E <count>` | Max continuous read errors |
| `-m <size>` | Minimum area size to copy |
| `-k <ratio>` | Skip ratio for bypassing the start of bad regions (default: 1/3) |
| `-s <size>` | Sector size (default: 512) |
| `-b <size>` | Buffer size (default: 32M) |
| `-f <size>` | Max forward copy segment size (default: 4M) |

Size values accept SI suffixes (`k`, `m`, `g`, `t` for powers of 1000) or binary suffixes (`K`, `M`, `G`, `T` for powers of 1024). Decimal values are supported with suffixes, e.g. `1.5G`.

## Example

```sh
# First pass — recover as much as possible, stop after 20 errors
# (this already copied 99% of recoverable data for me)
rescuecopy -d -e 20 /dev/sdX recovered.img

# Simply re-run to resume after failure or interruption, with different options if desired
rescuecopy [...] /dev/sdX recovered.img

# If the device eventually starts returning errors on everything,
# you can power-cycle it, restore the backup metadata, and resume trying to read:
rescuecopy -d -e 20 -E 10 /dev/sdX recovered.img
# power-cycle device, then try another round, retrying sectors incorrectly marked unreadable
cp recovered.img.rescue.bak recovered.img.rescue
rescuecopy -d -e 20 -E 10 /dev/sdX recovered.img
```
