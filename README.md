# Proyecto de Sistemas Operativos
## Proyecto 4: Copy-on-Write y MiniDB (xv6)

### Integrantes

- Sara Ruiz
- Jose Mendoza
- Pablo Alvarez

### Curso

Sistemas Operativos

### Semestre

2026-2

---

# Descripción

**Parte A — Copy-on-Write.** `fork()` ya no copia la memoria del padre. `uvmcopy()` mapea en el hijo las mismas páginas físicas del padre; a las que eran escribibles les quita `PTE_W` y les pone `PTE_COW` (bit 8, reservado al software) en **las dos** tablas de páginas, y suma una referencia a cada página física. Cada página física tiene un conteo de referencias en `kalloc.c`, protegido por su propio spinlock: `kalloc()` lo deja en 1, `krefinc()` suma y `kfree()` resta y solo devuelve la página a la lista libre cuando llega a 0. Una escritura sobre una página CoW produce un fallo de página (`scause` 15) que atiende `cowfault()`: si la página tiene más de una referencia, se reserva una página nueva, se copia y se mapea con `PTE_W`; si solo queda una referencia, se le devuelve `PTE_W` sin copiar. `copyout()` llama a `cowfault()` antes de escribir en memoria de usuario, porque el kernel escribe sin pasar por la MMU del usuario.

**Parte B — `cowstat()`.** Llamada al sistema nueva (número 25) que devuelve las páginas libres del sistema, las páginas con más de una referencia y los contadores `cowfaults` / `cowcopies` del proceso que llama (campos nuevos de `struct proc`, puestos en 0 en `allocproc()`).

**Parte C — MiniDB.** Se completó `minidb snapshot M`: el padre calcula el checksum y el saldo total de la tabla, limpia los bits `PTE_A` con `pgaccess()` y hace `fork()`. El hijo escribe `snap.db` (encabezado + 8192 cuentas) directamente desde la tabla y termina; el padre, sin esperarlo, aplica `M` transferencias. Al final el padre reporta las páginas tocadas y los fallos/copias CoW, espera al hijo, relee y verifica el archivo y verifica que el saldo total se conservó. `minidb consulta K` y `minidb mapa` son las del esqueleto.

---

# Archivos modificados

| Archivo | Qué se cambió y por qué |
|---|---|
| `kernel/riscv.h` | `#define PTE_COW (1L << 8)`: bit RSW que distingue "no escribible por ahora (CoW)" de "no escribible nunca" (código, guarda de pila, `USYSCALL`). |
| `kernel/kalloc.c` | Arreglo `kref.count[]` con un contador por página física entre `KERNBASE` y `PHYSTOP` y su propio spinlock `kref.lock`. `freerange()` pone cada página en 1 antes de `kfree()`; `kalloc()` deja la página en 1; `krefinc()`, `krefcount()`; `kfree()` solo libera al llegar a 0. `kfreepages()` (contador `nfree` bajo `kmem.lock`) y `ksharedpages()` (páginas con conteo > 1) para `cowstat()`. Nunca se tienen los dos locks a la vez. |
| `kernel/vm.c` | `uvmcopy()` comparte en vez de copiar y marca CoW solo lo que era escribible; salta páginas perezosas sin memoria. `cowfault()` nueva. `copyout()` resuelve la página CoW antes de escribir y relee la dirección física después. |
| `kernel/trap.c` | `usertrap()`: un fallo de escritura (`scause` 15) se entrega primero a `cowfault()`; si no es CoW se intenta `vmfault()` (páginas perezosas); si nada lo resuelve, `setkilled()`, nunca `panic()`. |
| `kernel/cowstat.h` | Nuevo: `struct cowstat`. |
| `kernel/proc.h`, `kernel/proc.c` | Campos `cowfaults` y `cowcopies` en `struct proc`; `allocproc()` los pone en 0. |
| `kernel/sysproc.c` | `sys_cowstat()`: llena la estructura y la copia al usuario con `copyout()`. |
| `kernel/syscall.h`, `kernel/syscall.c` | Registro de `SYS_cowstat` (25). |
| `kernel/defs.h` | Prototipos de `krefinc()`, `krefcount()`, `kfreepages()`, `ksharedpages()` y `cowfault()`. |
| `user/user.h`, `user/usys.pl` | Declaración de `cowstat()` y su stub de usuario. |
| `user/minidb.c` | `guardar_snapshot()` y `snapshot()` (Parte C.2), funciones auxiliares `contar_bits()` y `saldo_total()`, e `#include "kernel/stat.h"` para `fstat()`. |
| `Makefile` | `_cowtest` y `_minidb` en `UPROGS`. |

---

# Integración con xv6

1. Clonar xv6-riscv, hacer `git switch -c proyecto4 06aad25` y aplicar `git apply base_proyecto3.patch`.
2. Copiar los archivos entregados en sus rutas.
3. Copiar los archivos del docente: `user/cowtest.c`, `user/minidb.h`, `user/pgtbltest.c` y `user/usertests.c`.
4. Compilar con `make qemu CPUS=3`.
5. Ejecutar `cowtest`, `minidb consulta 3`, `minidb snapshot 2000`, `pgtbltest` y `usertests -q`.

---

# Respuestas a las preguntas

## 1. PTE antes y después de una escritura CoW

En la salida del comando `minidb mapa`, la primera página de la tabla de la base de datos se ubica en la dirección virtual `0x6000`:

* **Antes de escribir:** `pte 0x0000000021fd11d3` (Dirección física / PA: `0x87f44000`)
* **Después de escribir:** `pte 0x0000000021fb28d7` (Dirección física / PA: `0x87eca000`)

### Decodificación de los bits de la PTE (RISC-V Sv39)

| Bit | Nombre | Antes (`0x1d3`) | Después (`0x0d7`) | Significado |
| :--- | :--- | :---: | :---: | :--- |
| **V** | Valid | **1** | **1** | Entrada válida en la tabla de páginas. |
| **R** | Read | **1** | **1** | Permiso de lectura concedido. |
| **W** | Write | **0** | **1** | **Cambio:** Se restaura el permiso de escritura. |
| **X** | Execute | **0** | **0** | No es código ejecutable. |
| **U** | User | **1** | **1** | Accesible en modo usuario. |
| **A** | Accessed | **1** | **1** | La CPU accedió a la página. |
| **D** | Dirty | **1** | **1** | La página fue modificada. |
| **COW** | Copy-on-Write (bit 8) | **1** | **0** | **Cambio:** Se elimina la marca de CoW. |

### Explicación del cambio de dirección física en el hijo

Al ejecutar `fork()`, la función `uvmcopy()` compartió la página física `0x87f44000` entre el padre y el hijo, removiendo el permiso de escritura (`W=0`) y activando la marca Copy-on-Write (`COW=1`).

Cuando el hijo intentó escribir en la dirección virtual `0x6000`:

1. El hardware de la MMU detectó `W=0` e interrumpió la ejecución generando un fallo de página por escritura (`scause = 15`).
2. El manejador `usertrap()` capturó la trampa e invocó la función `cowfault()`.
3. Al consultar el contador de referencias de la página original (`0x87f44000`), `cowfault()` comprobó que la página estaba compartida (`refcount > 1`).
4. Por esta razón, reservó una **nueva página física** (`0x87eca000`) mediante `kalloc()`, copió los 4096 bytes de datos y decrementó en 1 el contador de referencias de la página vieja usando `kfree()`.
5. Finalmente, actualizó la PTE del hijo para mapear la nueva dirección `0x87eca000`, asignándole `W=1` y limpiando `COW=0`.

### Estado de la PTE en el proceso padre

En la tabla del proceso padre **no ocurre ningún cambio en sus bits ni en su dirección física** en el momento en que el hijo escribe:

* Su PTE sigue apuntando a la página física original **`0x87f44000`**.
* Mantiene sus permisos en **`W=0`** y **`COW=1`**.
* El único efecto a nivel de kernel es que el contador de referencias de `0x87f44000` bajó de 2 a 1. Si el padre intenta escribir después, `cowfault()` verá que `refcount == 1` y le devolverá el permiso `W=1` sobre esa misma página física sin volver a duplicar memoria.

---

## 2. Páginas sin PTE_W y USYSCALL en fork()

### Páginas de código y página de guarda (Guard Page)

* **Comportamiento en `fork()`:** Durante la clonación del proceso en `uvmcopy()`, el kernel únicamente aplica la lógica CoW a las entradas que tienen activo el permiso de escritura (`PTE_W`). Las páginas de código (que son solo lectura/ejecutables) y la página de guarda del stack (que no tiene permisos `PTE_U` ni `PTE_W`) se mapean en el proceso hijo conservando sus permisos originales, sin marcar el bit `PTE_COW`.
* **¿Por qué no se marcan como CoW?**: CoW está diseñado únicamente para páginas que los procesos tienen derecho a modificar legítimamente. Si el código o la página de guarda se marcaran como CoW, un intento erróneo o malicioso de escritura no generaría un fallo de segmentación (Segmentation Fault), sino que `cowfault()` interceptaría la trampa, crearía una copia física privada y le otorgaría permisos de escritura (`PTE_W`). Esto rompería la protección del sistema, permitiendo a un proceso modificar sus propias instrucciones o corromper memoria al desbordar la pila sobre la página de guarda.

### Manejo de `USYSCALL`

* **Comportamiento en `fork()`:** La página `USYSCALL` es una región compartida de solo lectura en espacio de usuario (`PTE_R` y `PTE_U`, pero sin `PTE_W`). Dado que el usuario nunca escribe en ella, `uvmcopy()` no le asigna el bit `PTE_COW`. Al hacer `fork()`, el kernel asigna una nueva página física independiente para el proceso hijo que apunta a su propia estructura `struct usyscall`.
* **¿Qué fallaría en `ugetpid()` si se compartiera?**: La función `ugetpid()` es una optimización en espacio de usuario que lee el PID del proceso directamente desde la página `USYSCALL` para evitar el costo de una trampa al kernel. Si el padre y el hijo compartieran la misma página física de `USYSCALL`, ambos leerían siempre el PID del proceso padre. Como consecuencia, la llamada `ugetpid()` en el hijo devolvería un PID incorrecto, rompiendo la identidad del proceso y haciendo fallar los tests del sistema.

---

## 3. Carrera en el conteo de referencias

Cuando múltiples CPUs leen y modifican el contador de referencias de una página física sin la protección de un `spinlock`, las operaciones de lectura, modificación y escritura dejan de ser atómicas, generando condiciones de carrera críticas.

### Escenario A: Pérdida de una página para siempre (Fuga de memoria / Memory Leak)

Supongamos que una página física `pa` está compartida por dos procesos (Padre e Hijo) y su contador es `count = 2`. Ambos procesos finalizan al mismo tiempo en CPUs distintas y llaman a `kfree(pa)`:

1. **Tiempo T1 (CPU 0 - Padre):** Ejecuta `kfree(pa)` y lee la memoria: `count = 2`.
2. **Tiempo T2 (CPU 1 - Hijo):** Ejecuta `kfree(pa)` de forma simultánea y lee la misma memoria: `count = 2`.
3. **Tiempo T3 (CPU 0):** Calcula `2 - 1 = 1`, escribe `count = 1` y verifica si `count == 0` (falso, no libera a `freelist`).
4. **Tiempo T4 (CPU 1):** Calcula `2 - 1 = 1` (basado en su lectura de T2), escribe `count = 1` y verifica si `count == 0` (falso, no libera a `freelist`).

**Resultado:** Ambos procesos terminaron y liberaron la página, pero el contador quedó en `1` en lugar de `0`. La página física queda "huérfana": ningún proceso la está usando y nunca volverá a la lista de páginas libres (`freelist`), perdiéndose memoria para siempre.

### Escenario B: Liberar una página que otro proceso todavía usa (Uso después de liberación / Use-After-Free)

Supongamos que una página física `pa` tiene `count = 1`. El Proceso A en CPU 0 la está liberando con `kfree(pa)` mientras el Proceso B en CPU 1 intenta duplicarla haciendo `fork()` con `krefinc(pa)`:

1. **Tiempo T1 (CPU 0 - `kfree`):** Lee `count = 1`.
2. **Tiempo T2 (CPU 1 - `krefinc`):** Lee `count = 1`.
3. **Tiempo T3 (CPU 0 - `kfree`):** Decrementa a `0`, detecta `count == 0` y coloca la página física en la `freelist`.
4. **Tiempo T4 (CPU 1 - `krefinc`):** Incrementa su lectura previa (`1 + 1 = 2`) y escribe `count = 2`.
5. **Tiempo T5:** Un Proceso C solicita memoria con `kalloc()`, recibe la página recién devuelta a la `freelist` y empieza a escribir datos en ella.

**Resultado:** La página física fue reciclada por el asignador mientras el Proceso B aún la tiene mapeada en su tabla de páginas. Las escrituras del Proceso C corromperán la memoria del Proceso B, causando fallos impredecibles o violaciones de seguridad.

---

## 4. Consistencia del snapshot

**¿Por qué `snap.db` tiene el checksum de antes del fork aunque el padre modificó la tabla mientras el hijo escribía?**
Porque `fork()` no copia la tabla: las 64 páginas físicas quedan compartidas entre padre e hijo, sin `PTE_W` y con `PTE_COW` en **las dos** tablas de páginas. El hijo solo **lee** la tabla: `write()` la recorre con `copyin()`, que lee la memoria del usuario sin escribirla, así que el hijo nunca provoca un fallo CoW sobre la tabla y sigue viendo las páginas físicas originales. Cuando el padre escribe una cuenta, el fallo de escritura lo provoca **el padre**, y es el padre quien recibe la página nueva: `cowfault()` reserva otra página, copia el contenido, se la mapea al padre con `PTE_W` y baja el conteo de la original. La página original queda con una sola referencia —la del hijo— y con el contenido exacto del instante del `fork()`. Así, el archivo es una foto de un único instante: no puede mezclar cuentas de antes y de después de una transferencia, y por eso su checksum es el calculado antes del `fork()` (`0x6915D295`), aunque al final la tabla del padre tenga `0x4AB3E209`.

**¿Por qué `copias_cow` es menor que 64?**
Porque solo se copian las páginas en las que el padre escribe, y solo la primera vez (después de la copia la página ya es privada y escribible). Con `M = 2000`, el padre toca 30 de las 64 páginas (`paginas_tocadas=30`) y hace exactamente 30 copias. Son pocas por la forma del generador: 255 de cada 256 transferencias ocurren entre las `NACTIVAS = 1024` primeras cuentas, que ocupan solo las 8 primeras páginas (1024 × 32 B = 32 KB); las otras 22 páginas tocadas vienen de las pocas transferencias (≈ 2000/256 ≈ 8) que pueden caer en cualquier cuenta y tocan el origen y el destino. Las 34 páginas que el padre nunca escribe no se copian nunca: siguen compartidas hasta que el hijo termina. Sin CoW, `fork()` habría copiado las 64 páginas de una vez (más el resto del proceso).

**¿Qué pasaría con `copias_cow` si el padre esperara al hijo antes de aplicar las transferencias?**
Sería 0. Al terminar el hijo, `exit()` libera su tabla de páginas y el conteo de las 64 páginas baja de 2 a 1. Las PTEs del padre siguen sin `PTE_W` y con `PTE_COW`, así que la primera escritura en cada página **sí** produce un fallo (`fallos_cow` seguiría siendo 30), pero `cowfault()` ve que la página tiene una sola referencia y toma el camino barato: le devuelve `PTE_W`, le quita `PTE_COW` y no copia. A cambio se perdería el objetivo del diseño (el `BGSAVE` de Redis): la base de datos quedaría detenida mientras se escriben los 256 KB a disco.

## 5. Aceleración y planificador

Ticks de `minidb consulta K` medidos en WSL + QEMU (3 corridas; se toma la mediana; aceleración = mediana(K=1) / mediana(K)):

| K | `CPUS=1` (3 corridas) | mediana | `CPUS=3` (3 corridas) | mediana | aceleración `CPUS=3` |
|---|---|---|---|---|---|
| 1 | 32, 32, 32 | 32 | 32, 29, 30 | 30 | 1,00× |
| 2 | 39, 31, 32 | 32 | 18, 20, 18 | 18 | 1,67× |
| 3 | 33, 29, 28 | 29 | 19, 18, 14 | 18 | 1,67× (mejor corrida: 2,14×) |
| 4 | 34, 28, 28 | 28 | 21, 14, 14 | 14 | 2,14× |
| 8 | 33, 31, 29 | 31 | 14, 15, 17 | 15 | 2,00× |

Con `CPUS=1` los ticks quedan planos (28–34) para cualquier K. Con `CPUS=3` bajan de ≈ 30 a ≈ 14–18 y dejan de bajar a partir de K = 3–4. La aceleración es algo menor que el 2,5–3× de la solución de referencia porque los 3 hilos de QEMU comparten los núcleos con Windows y WSL, y un tick de xv6 es una unidad gruesa (±1 tick sobre ≈ 15 ya es un 7 %).

**De 1 a 3 trabajadores con `CPUS=3` sí mejora** porque la consulta es trabajo de CPU puro: `firma()` hace 10 000 rondas por cuenta, sin disco ni bloqueos, y los trabajadores no comparten nada que se escriba (con CoW solo leen la tabla, así que ni siquiera se copian páginas). El padre crea los K trabajadores **antes** de leer cualquier pipe, así que quedan todos `RUNNABLE` a la vez. El planificador de xv6 corre un `scheduler()` en cada hart, y cada uno recorre la tabla de procesos en Round Robin buscando un proceso `RUNNABLE`; con 3 harts, los 3 trabajadores corren de verdad al mismo tiempo, cada uno con ≈ 8192/3 ≈ 2730 cuentas, y el tiempo de pared cae cerca de un tercio.

**De 3 a 8 no mejora** porque el trabajo total es el mismo (8192 firmas) y las CPUs siguen siendo 3. Con 8 trabajadores, Round Robin solo reparte esas 3 CPUs en más turnos: en cada interrupción del temporizador el proceso en curso hace `yield()` y el hart toma el siguiente `RUNNABLE`, pero la suma de ciclos de CPU necesarios no cambia. Lo que se agrega es costo: 8 `fork()`, 8 pipes, 8 `wait()` y más cambios de contexto, por eso puede incluso empeorar un poco. La aceleración se detiene en el número de núcleos.

**Con `CPUS=1` no mejora** porque hay un solo hart y un solo `scheduler()`. Los K trabajadores existen a la vez, pero Round Robin los **intercala** (cada uno corre hasta el siguiente tick y cede) en vez de ejecutarlos en paralelo. El tiempo total es la suma del trabajo de todos más los cambios de contexto, así que los ticks se mantienen planos (o suben un poco) al crecer K. Más procesos no crean más CPU.

---

# Resultados de las pruebas

Generadas con `python3 ejecutar_pruebas.py` (3 CPUs en QEMU). Salida completa en `pruebas/`.

| Suite | Resultado | Detalle |
|---|---|---|
| `cowtest` | PASÓ | 11 de 11 pruebas OK. Los mensajes `usertrap(): unexpected scause 0xf` vienen de `solo_lectura` y `sin_memoria`, que provocan escrituras inválidas o agotan la memoria a propósito. |
| `minidb` | PASÓ | Revisión automática OK. `consulta 1/2/3/8`: firma `0x2DD2F610` y las 8 sucursales correctas. `snapshot 2000`: `aplicadas=1978 rechazadas=22`, `paginas_tocadas=30 fallos_cow=30 copias_cow=30`, `snap.db` de 262160 bytes con checksum `0x6915D295`, `checksum_actual=0x4AB3E209`, saldo conservado. `snapshot 0`: 0 copias. `mapa`: la PTE pasa de `PTE_COW` sin `PTE_W` a `PTE_W` sin `PTE_COW`, con otra dirección física. |
| `pgtbltest` | PASÓ | 11 de 11 pruebas OK (regresión del Proyecto 3). |
| `usertests -q` | PASÓ | `ALL TESTS PASSED`. |

El kernel compila sin advertencias del compilador (`-Wall -Werror`); `ld` solo muestra el aviso de segmento RWX que ya traía xv6 en la base.

---

# Decisiones de diseño

- **Un spinlock propio para el conteo** (`kref.lock`), separado de `kmem.lock`, y nunca se toman los dos a la vez: `kfree()` decrementa y suelta `kref.lock` antes de tomar `kmem.lock`, y `cowfault()` llama a `kalloc()`/`kfree()` sin tener ningún lock del conteo. Así no hay *deadlock*.
- **`PTE_COW` solo en lo que era escribible.** El código, la guarda de pila y `USYSCALL` no se marcan, para que una escritura ahí siga matando al proceso. `USYSCALL` y el trapframe no se comparten porque están por encima de `sz` y cada proceso los crea en `proc_pagetable()`.
- **Una sola referencia no copia**, pero cuenta como fallo (`cowfaults++` sin `cowcopies++`).
- **Snapshot.** El hijo escribe directo desde `tabla` (sin buffer intermedio) para que la consistencia venga de CoW y no de una copia hecha por el programa. Se usa `O_TRUNC` para no dejar restos de un `snap.db` anterior. El buffer para releer el archivo (256 KB) se pide con `sbrk()` porque la pila de usuario es de una página. `pgaccess()` se llama antes del `fork()` para limpiar los `PTE_A` que dejaron `cargar_tabla()` y el checksum. `cowstat()` se llama dos veces antes de las transferencias para que el fallo CoW de la página de pila donde `copyout()` escribe la estructura no se cuente como parte de las transferencias.

---

# Limitaciones conocidas

- El conteo de referencias usa `uchar` por página: alcanza porque una página de usuario la comparten como máximo `NPROC` = 64 procesos.
- La aceleración medida depende de la carga de la máquina anfitriona (WSL/QEMU); en corridas distintas varía algunos ticks.
- Como pide el enunciado, las páginas de las tablas de páginas no se comparten con CoW: cada trabajador de `minidb consulta` sigue gastando ≈ 9 páginas (≈ 5 páginas de tabla de páginas, trapframe, `USYSCALL`, el buffer de su pipe y la copia CoW de su página de pila).

---

# Declaración de uso de IA

Se usó Claude (Anthropic) como apoyo para revisar el código del kernel, completar y depurar `snapshot()` de MiniDB, automatizar la corrida de los experimentos y redactar borradores de las respuestas 4 y 5. Todo el contenido fue revisado por el grupo. Los integrantes asumen plena responsabilidad académica sobre el contenido entregado y pueden explicar cada línea del código.