#!/usr/bin/env python3
"""Apply a source patch as LF text, including after a Windows CRLF checkout."""
import argparse
from pathlib import Path
import subprocess

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory')
    parser.add_argument('patch',type=Path)
    parser.add_argument('--reverse-check',action='store_true')
    args=parser.parse_args()
    command=['git','-C',args.directory,'apply']
    if args.reverse_check:command += ['--reverse','--check']
    data=args.patch.read_bytes().replace(b'\r\n',b'\n')
    return subprocess.run(command+['-'],input=data).returncode

if __name__=='__main__':
    raise SystemExit(main())
