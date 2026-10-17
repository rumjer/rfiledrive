# fileserver

fileserver is a simple browser-based file manager, written in c++, great for remote access file management. useful for home-servers.
users browse files, preview images and videos (has .heic preview support), create folders/directories, upload/download files, delete files, etc (more below)

it allows you to:

- create folders
- upload files
- download files
- delete files with a trash function
- see storage information (space, etc)

it includes a trash function which keeps files for 48 hours before deletion and storage info
it also shows your path and includes a safepath and other stuff

i use it on my personal debian server to which i connect to via tailscale to upload projects from my pc to my laptop
i made it as a learning project for a new library
it was mostly for my need to share files and keep files easily

i only coded the important functions in mainfunctions.cpp and vibecoded the functions for the trash and storage info.
also the entire frontend is ai generated. i havent even touched a line of html or json.
unlike the trash functions the frontend is fully ai generated and not so readable like the cpp code.

the gui is very easy to use and convenient

## how to run

compile with make:
```bash
make
./fileserver
```
then you can open http://localhost:8080 in browser
