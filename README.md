<img width="1716" height="890" alt="example" src="https://github.com/user-attachments/assets/517ee753-b833-4ccd-9a34-fc746f46612e" />

# fileserver

a simple browser-based file manager written in c++, made for remote access to the files on a home server
open it in your browser, then browse, upload, download, rename, move and delete files and folders, like a self hosted google drive

i run it on my own debian server and connect to it over tailscale, so i can move projects and files between my pc and my laptop
i made it as a learning project to try out a new library ([cpp-httplib](https://github.com/yhirose/cpp-httplib))

## features

- browse folders and files, with a clickable path at the top
- upload files and whole folders (button or drag and drop)
- download files
- rename and move files and folders
- preview images, videos and .heic photos in the browser
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

**storage details** (which folders use the most space)

<img width="765" height="623" alt="storageinfo" src="https://github.com/user-attachments/assets/52095efa-f21a-43a1-bb98-bcda406ce81c" />

## how to run

### 1. install what you need

you need a c++17 compiler, `make`, `pkg-config` and the libheif dev files (used for the .heic previews)

```bash
# example for apt and apk (since they are mostly used for servers)

doas apt install libheif-examples libheif1
doas apk add libheif-dev

```

### 2. download and build

```bash
git clone https://github.com/rumjerr/fileserver.git
cd fileserver
make
```

(this creates the `fileserver` program in the same folder)

### 3. set up the folders

the server needs a folder to serve files from, and a `www` folder with the web page in it:

```bash
mkdir -p ~/files
mkdir -p www
cp index.html www/
```

### 4. start it

```bash
FILES_ROOT=$HOME/files FILES_HOST=127.0.0.1 ./fileserver
```

now open http://localhost:8080 in your browser.

the trash is stored next to your files folder, named after it with `-trash` added (here: `~/files-trash`), so the user running the server must be allowed to create folders in the parent directory.

### settings

everything is configured with environment variables:

| variable | default | what it does |
| --- | --- | --- |
| `FILES_ROOT` | `/srv/files` | the folder whose files are shown and managed |
| `FILES_WWW` | `./www` | the folder that contains the web page (`index.html`) |
| `FILES_HOST` | `0.0.0.0` | the address to listen on (`127.0.0.1` = this computer only) |
| `FILES_PORT` | `8080` | the port to listen on |

### run it as a sysd service (optional)

on a server you can start it automatically with systemd. save this as `/etc/systemd/system/fileserver.service` and change the user and paths to match yours

(this is what i use on my debian server)

```ini
[Unit]
Description=File server
After=network-online.target

[Service]
User=youruser
WorkingDirectory=/home/youruser/fileserver
Environment=FILES_ROOT=/srv/files
Environment=FILES_WWW=/home/youruser/fileserver/www
Environment=FILES_HOST=127.0.0.1
ExecStart=/home/youruser/fileserver/fileserver
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

then reload

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now fileserver
```

## security notice for your files

**there is no login.** anyone who can reach the port can see, change and delete everything in the files folder
only run it on a network you trust: on your own computer (`FILES_HOST=127.0.0.1`), inside a vpn like tailscale like i do, or behind something that adds a login. for tailscale, you need to allow users to be able to connect to your hosted machine. this is done through a link. if youre not connected to the network via tailscale, you cannot access the files

dont open the port to the internet, anyone will be able to edit your files

## limits

- empty folders are not uploaded (a folder is only created when a file is saved into it), i have no idea why you'd want to upload an empty folder anyways, but if you want an empty folder you can create one with the intended 'new folder' button
- only .heic / .heif files get a converted preview, other types open as they are. but for most of your files, they can easily be viewed without needing to download them

## how it works

| file | what it does |
| --- | --- |
| `main.cpp` | the web routes (`/api/list`, `/api/upload`, `/api/download`, `/api/rename`, `/api/move`, `/api/delete`, `/api/trash/...`, `/api/storage`, ...) |
| `mainfunctions.cpp` / `.h` | the file logic: safe paths, listing, upload, trash, storage info, .heic conversion |
| `index.html` | the whole web interface |
| `httplib.h`, `json.hpp`, `stb_image_write.h` | libraries that are included in the repo: [cpp-httplib](https://github.com/yhirose/cpp-httplib), [nlohmann/json](https://github.com/nlohmann/json), [stb](https://github.com/nothings/stb) |

.heic files are decoded with [libheif](https://github.com/strukturag/libheif) and sent to the browser as jpeg.

## additional info

i wrote the core file functions in `mainfunctions.cpp` (listing, upload, download, safe paths, rename and move).
the trash and storage info functions, some other small changes and the whole frontend (`index.html`) were vibecoded.
the frontend in particular is not very readable, since i havent written a single line of html myself. on the otherhand the backend has nicely listed comments for each function. so if youd like to edit some values, for example the purge function, you can make it so files delete after just 24h instead of 48h.
