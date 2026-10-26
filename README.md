<img width="1716" height="890" alt="example" src="https://github.com/user-attachments/assets/517ee753-b833-4ccd-9a34-fc746f46612e" />

# rFiledrive

a simple browser-based file manager and editor written in c++, made for remote access to the files on a home server
open it in your browser, then browse, upload, download, rename, move, view, edit and restore files and folders, like a self hosted google drive

i run it on my own debian server and connect to it over tailscale, so i can move projects and files between my pc and my laptop
i made it as a learning project to try out a new library ([cpp-httplib](https://github.com/yhirose/cpp-httplib))

## features

- browse folders and files, with a clickable path at the top (like with a desktop file manager)
- upload files and whole folders (button or drag and drop)
- download files
- rename and move files and folders
- preview images, videos, pdf, xls, xlsx, txt, csv, sqlite and others in the browser
- edit live from the browser files which are on the hosted machine (csv, txt, sqlite cells)
- history, previous file recovery: past versions of edited files are saved and restorable by a single click
- atomic saving & conflict protection: saved edits write to a temp file first so nothing gets corrupted, and warns you if the file changed on the server meanwhile
- trash: deleted items are kept for 48 hours, can be restored, and are then removed automatically
- storage details: free space and the biggest folders
- paths are checked so nothing outside the files folder can be reached (`safePath`)

## screenshots

**preview** (different functions/features showcased)

<img width="1701" height="866" alt="preview" src="https://github.com/user-attachments/assets/27ae100d-8f9c-4e93-bad6-7aad5ae32a5d" />

**move** (pick a destination folder)

<img width="1730" height="867" alt="move" src="https://github.com/user-attachments/assets/6102a7e8-03f6-4c63-9c66-e3a7c8dd95ad" />

**trash** (restore items or wipe/delete them)

<img width="1707" height="878" alt="trash" src="https://github.com/user-attachments/assets/646b0f2d-8815-4285-94b2-ece82fe13cc9" />

**live editing** (edit files from your browser)

<img width="1521" height="891" alt="image" src="https://github.com/user-attachments/assets/3f96bce8-aa9d-41f5-9cd0-7d5f4d1e4640" />

**restoring** (restore previous file versions)

<img width="1503" height="907" alt="image" src="https://github.com/user-attachments/assets/e5c237ab-c1d6-44e6-9e51-b83286699a74" />

**storage details** (which folders use the most space)

<img width="765" height="623" alt="storageinfo" src="https://github.com/user-attachments/assets/52095efa-f21a-43a1-bb98-bcda406ce81c" />

## how to run

### 1. install what you need

you need a c++17 compiler, `make`, `pkg-config`, the libheif dev files (used for the .heic previews), and sqlite3 dev files:

```bash
# example for apt and apk (since they are mostly used for servers)

sudo apt install build-essential pkg-config libheif-dev libsqlite3-dev
doas apk add build-base pkgconf libheif-dev sqlite-dev
```

### 2. download and build

```bash
git clone https://github.com/rumjer/rfiledrive.git
cd rfiledrive
make
```

(this creates the `rfiledrive` program in the same folder)

### 3. set up the folders

the server needs a folder to serve files from, and a `www` folder with the web files in it:

```bash
mkdir -p ~/files
mkdir -p www
cp index.html liveedit.js liveedit.css www/
cp -r vendor www/
```

### 4. start it

```bash
FILES_ROOT=$HOME/files FILES_HOST=127.0.0.1 ./rfiledrive
```

now open http://localhost:8080 in your browser.

- the trash is stored next to your files folder, named after it with `-trash` added (here: `~/files-trash`).
- previous file versions are stored next to your files folder with `-versions` added (here: `~/files-versions`).
- the user running the server must be allowed to create folders in the parent directory.

### settings

everything is configured with environment variables:

| variable | default | what it does |
| --- | --- | --- |
| `FILES_ROOT` | `/srv/files` | the folder whose files are shown and managed |
| `FILES_WWW` | `./www` | the folder that contains the web page (`index.html`, etc.) |
| `FILES_HOST` | `0.0.0.0` | the address to listen on (`127.0.0.1` = this computer only) |
| `FILES_PORT` | `8080` | the port to listen on |

### run it as a sysd service (optional)

on a server you can start it automatically with systemd. save this as `/etc/systemd/system/rfiledrive.service` and change the user and paths to match yours (this is a good qol addition)

(this is what i use on my debian server)

```ini
[Unit]
Description=rFiledrive File Server
After=network-online.target

[Service]
User=youruser
WorkingDirectory=/home/youruser/rfiledrive
Environment=FILES_ROOT=/srv/files
Environment=FILES_WWW=/home/youruser/rfiledrive/www
Environment=FILES_HOST=127.0.0.1
ExecStart=/home/youruser/rfiledrive/rfiledrive
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

then reload

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now rfiledrive
```

## security notice for your files

**there is no login.** anyone who can reach the port can see, change and delete everything in the files folder
only run it on a network you trust: on your own computer (`FILES_HOST=127.0.0.1`), inside a vpn like tailscale like i do, or behind something that adds a login. for tailscale, you need to allow users to be able to connect to your hosted machine. this is done through a link. if youre not connected to the network via tailscale, you cannot access the files

dont open the port to the internet, anyone will be able to edit your files

## limits

- empty folders are not uploaded (a folder is only created when a file is saved into it), i have no idea why you'd want to upload an empty folder anyways, but if you want an empty folder you can create one with the intended 'new folder' button
- only .heic / .heif files get a converted preview, other types open as they are. but for most of your files, they can easily be viewed without needing to download them
- version history keeps up to 25 versions per file by default so your drive doesnt fill up

## how it works

| file | what it does |
| --- | --- |
| `main.cpp` | the web routes (`/api/list`, `/api/upload`, `/api/download`, `/api/rename`, `/api/move`, `/api/delete`, `/api/trash/...`, `/api/storage`, `/api/save`, `/api/db/...`) |
| `mainfunctions.cpp` / `.h` | the file logic: safe paths, listing, upload, trash, storage info, .heic conversion |
| `liveedit.cpp` / `.h` | safe saving logic: writing to temp files, version backups in `-versions/`, conflict tokens |
| `sqlite_db.cpp` / `.h` | generic sqlite database endpoints (read tables, view rows, edit cells) |
| `index.html` | the main web interface |
| `liveedit.js` / `.css` | the viewer/editor window: handles pdf, excel, csv, text, and the sqlite grid |
| `vendor/` | bundled libraries for the browser: pdf.js for reading pdfs, sheetjs for excel and stuff |
| `httplib.h`, `json.hpp`, `stb_image_write.h` | libraries that are included in the repo: [cpp-httplib](https://github.com/yhirose/cpp-httplib), [nlohmann/json](https://github.com/nlohmann/json), [stb](https://github.com/nothings/stb) |

.heic files are decoded with [libheif](https://github.com/strukturag/libheif) and sent to the browser as jpeg.

## additional info

i wrote the core file functions in `mainfunctions.cpp` (listing, upload, download, safe paths, rename and move).
the trash and storage info functions, a good part of live editing, and the frontend were vibecoded.
the frontend in particular is not very readable, since i havent written a single line of html myself. on the otherhand the backend has nicely listed comments for each function. so if youd like to edit some values, for example the purge function, you can make it so files delete after just 24h instead of 48h.
