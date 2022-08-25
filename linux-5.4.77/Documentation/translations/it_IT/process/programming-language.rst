.. include:: ../disclaimer-ita.rst

:Original: :ref:`Documentation/process/programming-language.rst <programming_language>`
:Translator: Federico Vaga <federico.vaga@vaga.pv.it>

.. _it_programming_language:

Linguaggio di programmazione
============================

Il kernel è scritto nel linguaggio di programmazione C [c-language]_.
Più precisamente, il kernel viene compilato con ``gcc`` [gcc]_ usando
l'opzione ``-std=gnu89`` [gcc-c-dialect-options]_: il dialetto GNU
dello standard ISO C90 (con l'aggiunta di alcune funzionalità da C99)

Questo dialetto contiene diverse estensioni al linguaggio [gnu-extensions]_,
e molte di queste vengono usate sistematicamente dal kernel.

Il kernel offre un certo livello di supporto per la compilazione con ``clang``
[clang]_ e ``icc`` [icc]_ su diverse architetture, tuttavia in questo momento
il supporto non è completo e richiede delle patch aggiuntive.

Attributi
---------

Una delle estensioni più comuni e usate nel kernel sono gli attributi
[gcc-attribute-syntax]_. Gli attributi permettono di aggiungere una semantica,
definita dell'implementazione, alle entità del linguaggio (come le variabili,
le funzioni o i tipi) senza dover fare importanti modifiche sintattiche al
linguaggio stesso (come l'aggiunta di nuove parole chiave) [n2049]_.

In alcuni casi, gli attributi sono opzionali (ovvero un compilatore che non
dovesse supportarli dovrebbe produrre comunque codice corretto, anche se
più lento o che non esegue controlli aggiuntivi durante la compilazione).

Il kernel definisce alcune pseudo parole chiave (per esempio ``__pure``)
in alternativa alla sintassi GNU per gli attributi (per esempio
``__attribute__((__pure__))``) allo scopo di mostrare quali funzionalità si
possono usare e/o per accorciare il codice.

Per maggiori informazioni consultate il file d'intestazione
``include/linux/compiler_attributes.h``.

.. [c-language] http://www.open-std.org/jtc1/sc22/wg14/www/standards
.. [gcc] https://gcc.gnu.org
.. [clang] https://clang.llvm.org
.. [icc] https://software.intel.com/en-us/c-compilers
.. [gcc-c-dialect-options] https://gcc.gnu.org/onlinedocs/gcc/C-Dialect-Options.html
.. [gnu-extensions] https://gcc.gnu.org/onlinedocs/gcc/C-Extensions.html
.. [gcc-attribute-syntax] https://gcc.gnu.org/onlinedocs/gcc/Attribute-Syntax.html
.. [n2049] http://www.open-std.org/jtc1/sc22/wg14/www/docs/n2049.pdf
