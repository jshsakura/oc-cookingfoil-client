<div align="center">
  <h1><img src="brand/cookingfoil-readme-256.png" alt="CookingFoil" width="192" height="192" /></h1>
  <p><strong>Tus archivos en tu servidor. Tu biblioteca en tu consola.</strong></p>
  <p>Un cliente homebrew personal para juegos, apps homebrew, partidas guardadas y parches.</p>
  <p>
  <a href="https://jshsakura.github.io/oc-cookingfoil/?lang=en"><img src="brand/buttons/website-es.svg" alt="Sitio web" width="160" height="44" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil/blob/main/README.md#connecting-a-client"><img src="brand/buttons/connect-es.svg" alt="Conectar consola" width="160" height="44" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil/blob/main/README.md#quick-start"><img src="brand/buttons/setup-es.svg" alt="Instalación" width="160" height="44" /></a>
  <a href="docs/README.md"><img src="brand/buttons/docs-es.svg" alt="Documentación" width="160" height="44" /></a>
  </p>
  <p><a href="README.md">English</a> · <a href="README.ko.md">한국어</a> · <a href="README.ja.md">日本語</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.es.md">Español</a></p>
  <a href="https://github.com/jshsakura/oc-cookingfoil-client/actions/workflows/ci.yml"><img src="https://github.com/jshsakura/oc-cookingfoil-client/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil-client/releases"><img src="https://img.shields.io/github/v/release/jshsakura/oc-cookingfoil-client?color=FFC23D" alt="GitHub release" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-16161A" alt="GPL-3.0-or-later" /></a>
</div>

![Pantalla de inicio de CookingFoil](docs/screenshots/en/01-home.png)

Conéctate a tu propio [servidor CookingFoil](https://github.com/jshsakura/oc-cookingfoil), explora tus archivos
y elige qué instalar. Puedes ver varios servidores a la vez, comparar las versiones instaladas con las
actualizaciones y retomar las descargas que se cortaron.

## Echa un vistazo

| Detalle del juego | Descargas |
|---|---|
| ![Detalle del juego](docs/screenshots/en/02-detail.png) | ![Descargas](docs/screenshots/en/04-downloads.png) |
| Juegos instalados | Partidas guardadas y Dropbox |
| ![Juegos instalados](docs/screenshots/en/07-installed.png) | ![Partidas guardadas y Dropbox](docs/screenshots/en/08-dropbox-linked.png) |

Capturas de la interfaz real en inglés con datos de ejemplo. Los elementos sin icono usan las fichas con la inicial que dibuja la app.

## Elige lo que quieres. Del resto se encarga CookingFoil

| Lo que quieres hacer | Lo que hace CookingFoil |
|---|---|
| **Explorar e instalar** | Búsqueda, filtros, orden y detalles de cada juego. Eliges los archivos base, de actualización y DLC, y si van a la tarjeta SD o a la memoria de la consola. NSP, NSZ, XCI y XCZ se instalan con el motor de CyberFoil. |
| **Usar varios servidores** | Muestra sus catálogos juntos y, si varios tienen lo mismo, te deja elegir desde cuál descargar. |
| **Seguir una descarga** | Guarda la cola y el registro de descargas completadas, fallidas y canceladas. Las descargas compatibles se reanudan desde donde iban, esperan a que vuelva la conexión y mantienen la consola despierta. |
| **Buscar actualizaciones** | Compara los juegos instalados con las versiones del servidor. Las actualizaciones homebrew se comprueban con los hashes que da el servidor. |
| **Gestionar homebrew** | Instala y actualiza un NRO suelto o la carpeta de una app. Crea accesos directos en el menú HOME para las apps compatibles. |
| **Cuidar tus partidas** | Copia y restaura en la consola las partidas de cada usuario, importa las del servidor y sube o baja copias de Dropbox. Antes de restaurar se hace una copia para poder volver atrás. |
| **Aplicar parches** | Activa y desactiva parches de archivos del juego, trucos, IPS y FPSLocker. Revisa los conflictos de archivos y une los trucos por build ID. |
| **Jugar a lo retro** | Elige juegos por sistema y descárgalos con todo lo que necesitan. Usa los paquetes de RetroArch, los accesos directos y la información de RetroAchievements que ofrece el servidor. |
| **Hacer sitio** | Gestiona los juegos instalados y limpia los restos tras confirmarlo. Solo se borra contenido que ningún juego instalado usa. |

La interfaz está en coreano, inglés, japonés y chino, y se maneja con la pantalla táctil o con el mando. El homebrew,
los parches, lo retro y algunas integraciones aparecen según lo que admita el servidor. Tú decides cuándo aplicar
partidas y parches.

## Primeros pasos

1. **Prepara un servidor.** Sigue la [guía del servidor CookingFoil](https://github.com/jshsakura/oc-cookingfoil#readme) o usa un servidor al que tengas permiso de acceso.
2. **Copia el cliente.** Descarga [`cookingfoil.nro`](https://github.com/jshsakura/oc-cookingfoil-client/releases/latest/download/cookingfoil.nro) de la última versión, o desde la página web de tu servidor, y cópialo en `switch/cookingfoil/` de la tarjeta SD. No hace falta ningún archivo de configuración.
3. **Conéctate.** Abre la app desde el menú Homebrew y escribe la dirección en **Ajustes → Añadir servidor**. También sirve un `config.json` creado en el PC.
4. **Aprueba la consola.** Si el servidor pide aprobación, la consola muestra un código QR. Inicia sesión en el móvil, apruébala y la consola se conecta.

Necesitas una consola que pueda ejecutar homebrew. Usa el modo aplicación para lo que necesite permisos de la
consola, como gestionar partidas guardadas. Si el modo applet bloquea el acceso, la app explica cómo abrirla.

La configuración, la autenticación y el uso de varios servidores están en [Primeros pasos (en inglés)](docs/GETTING-STARTED.md).

## Desarrollo y vista previa

La versión de escritorio, en C++17 y SDL2, ejecuta la interfaz real en el PC. Puedes explorar datos de ejemplo
o conectar un servidor, y controlar la app desde el navegador.

```sh
git clone --recursive https://github.com/jshsakura/oc-cookingfoil-client
cd oc-cookingfoil-client
# Antes, instala las dependencias de escritorio que indica la guía de compilación.
make test host
./build-host/cookingfoil-host --fixture host/fixtures --lang en --serve 38520
```

Abre `http://localhost:38520/`. Las instalaciones en la consola, el acceso a las partidas y el borrado se comprueban
en el hardware. Las dependencias, la compilación para la consola y las capturas están en
[Compilar y previsualizar (en inglés)](docs/BUILDING.md).

## Más información

| Guía | Contenido |
|---|---|
| [Primeros pasos (en inglés)](docs/GETTING-STARTED.md) | Instalación, configuración, aprobación por QR, varios servidores y archivos de la SD |
| [Funciones (en inglés)](docs/FEATURES.md) | Descargas, homebrew, partidas, parches, retro y sus requisitos |
| [Compilar y previsualizar (en inglés)](docs/BUILDING.md) | Compilación de escritorio y de consola, pruebas y vista previa en el navegador |
| [Diseño (en inglés)](docs/DESIGN.md) | Disposición, foco, entrada y respuesta |
| [Textos y traducciones (en inglés)](docs/WRITING.md) | Claves de traducción en inglés, los cuatro idiomas y las reglas de redacción |
| [Normas de trabajo (en inglés)](docs/CONTRIBUTING-GUIDE.md) | Hilos, escritura segura de archivos, pruebas y pull requests |
| [Código de terceros (en inglés)](third_party/README.md) | Mantenimiento del motor de instalación y sus parches |

## Licencia y contenido

El cliente usa la licencia [GPL-3.0-or-later](LICENSE). El servidor, un proyecto aparte, usa la licencia MIT.
El cliente usa el motor de instalación de [CyberFoil](https://github.com/luketanti/CyberFoil), que deriva de
Awoo Installer y Tinfoil. Consulta los [avisos de terceros (en inglés)](THIRD_PARTY_NOTICES.md).

No incluye juegos, claves, archivos BIOS ni RetroArch. Usa solo contenido que poseas legalmente o que tengas permiso
para usar, y parches que se puedan distribuir.
