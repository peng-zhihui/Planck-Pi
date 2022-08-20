// SPDX-License-Identifier: GPL-2.0-only
/// Remove casting the values returned by memory allocation functions
/// like kmalloc, kzalloc, kmem_cache_alloc, kmem_cache_zalloc etc.
///
//# This makes an effort to find cases of casting of values returned by
//# malloc, calloc, kmalloc, kmalloc_array, kmalloc_node and removes
//# the casting as it is not required. The result in the patch case may
//# need some reformatting.
//
// Confidence: High
// Copyright: (C) 2014 Himangi Saraogi
// Copyright: (C) 2017 Himanshu Jha
// Comments:
// Options: --no-includes --include-headers
//

virtual context
virtual patch
virtual org
virtual report

@initialize:python@
@@
import re
pattern = '__'
m = re.compile(pattern)

@r1 depends on context || patch@
type T;
@@

  (T *)
  \(malloc\|calloc\|kmalloc\|kmalloc_array\|kmalloc_node\)(...)

//----------------------------------------------------------
//  For context mode
//----------------------------------------------------------

@script:python depends on context@
t << r1.T;
@@

if m.search(t) != None:
        cocci.include_match(False)

@depends on context && r1@
type r1.T;
@@

* (T *)
  \(malloc\|calloc\|kmalloc\|kmalloc_array\|kmalloc_node\)(...)

//----------------------------------------------------------
//  For patch mode
//----------------------------------------------------------

@script:python depends on patch@
t << r1.T;
@@

if m.search(t) != None:
        cocci.include_match(False)

@depends on patch && r1@
type r1.T;
@@

- (T *)
  \(malloc\|calloc\|kmalloc\|kmalloc_array\|kmalloc_node\)(...)

//----------------------------------------------------------
//  For org and report mode
//----------------------------------------------------------

@r2 depends on org || report@
type T;
position p;
@@

 (T@p *)
  \(malloc\|calloc\|kmalloc\|kmalloc_array\|kmalloc_node\)(...)

@script:python depends on org@
p << r2.p;
t << r2.T;
@@

if m.search(t) != None:
	cocci.include_match(False)
else:
	coccilib.org.print_safe_todo(p[0], t)

@script:python depends on report@
p << r2.p;
t << r2.T;
@@

if m.search(t) != None:
	cocci.include_match(False)
else:
	msg="WARNING: casting value returned by memory allocation function to (%s *) is useless." % (t)
	coccilib.report.print_report(p[0], msg)
