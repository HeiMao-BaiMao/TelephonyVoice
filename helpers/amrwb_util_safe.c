/*
 ** Copyright 2003-2010, VisualOn, Inc.
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **     http://www.apache.org/licenses/LICENSE-2.0
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */
/* Parent-side bounded replacement for amrwbenc/src/util.c. The vendored
 * Copy reads two elements past every even-length input, including immutable
 * isf_init during encoder creation. Straight loops preserve its forward-copy
 * semantics without speculative reads, and also make zero lengths safe. */
#include "typedef.h"
#include "basic_op.h"
void Set_zero(Word16 x[], Word16 length) {
    for (int i = 0; i < length; ++i) x[i] = 0;
}
void Copy(Word16 x[], Word16 y[], Word16 length) {
    for (int i = 0; i < length; ++i) y[i] = x[i];
}
