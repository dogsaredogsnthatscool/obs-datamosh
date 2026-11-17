# Face finder data

`lbpcascade_frontalface_opencv.xml` is OpenCV's LBP frontal face cascade (`data/lbpcascades/lbpcascade_frontalface.xml` in OpenCV; this copy is the one scikit-image ships). `build.sh` turns it into `build/inc/face-cascade.h` with `cascade2h.py`, and `src/facedetect.c` runs it.

OpenCV's cascade files are distributed under the Intel License Agreement For Open Source Computer Vision Library (BSD 3-clause style): redistribution in source and binary form is permitted provided the copyright notice, conditions and disclaimer are retained, and the names of Intel and the contributors are not used to endorse derived products. Copyright (C) 2000-2008, Intel Corporation; Copyright (C) 2009, Willow Garage Inc. The software is provided "as is" without warranty of any kind.
