﻿// LowPoly.cpp : Defines the entry point for the application.
//
#include <vector>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include "opencv2/core.hpp"
#include "opencv2/highgui.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "point.h"
#include "LowPoly.h"
#include "delauney.h"
#include "triangle.h"
#include "cvutil.h"
#include "cycleTimer.h"
#include <ctime>
#include <cstring>

#include <cuda.h>
#include <cuda_runtime.h>
#include <driver_functions.h>
#include <thrust/device_vector.h>
#include <thrust/host_vector.h>

using namespace std;
double total_comp_start = 0;

vector<Point> InputFromFile(char* filePath, int &numVertices, int &rows, int &cols)
{
    freopen(filePath, "r", stdin);
    vector<Point> ret;
    cin>>numVertices>>rows>>cols;
    for(int i=1; i<=numVertices; i++)
    {
        int x, y;
        cin>>x>>y;
        ret.emplace_back(x, y);
    }
    return ret;
}

vector<Point> InputFromImage(char* imgPath, int &numVertices, int &rows, int &cols, cv::Mat& img, float edgeP, float nonEdgeP, float boundP, float edgeThresh)
{
    // Read image, set rows and cols
    img = cv::imread(imgPath);
    if (!img.data)
    {
        printf("Error loading image %s\n", imgPath);
        exit(-1);
    }

    total_comp_start = CycleTimer::currentSeconds();

    rows = img.rows;
    cols = img.cols;
    int numPixel = rows * cols;
    printf("Img Size: %d x %d\n", rows, cols);
    // scale P to make result visually good
    edgeP *= 2160.f / rows;
    nonEdgeP *= 2160.f / rows;
    boundP *= 2160.f / rows;

    double grad_start = CycleTimer::currentSeconds();
    cv::Mat grad = getGrad(img);
    cout<<"Edge Detection Time: "<< (CycleTimer::currentSeconds() - grad_start) * 1000 <<"ms"<<endl;

    // // calculate threshold for selecting edge pixel and non-edge Pixel
    // int numEdgePix = 0;
    // for (int row = 0; row < rows; row++)
    // {
    //     for (int col = 0; col < cols; col++)
    //     {
    //         if (grad.at<float>(row, col) >= edgeThresh)
    //             numEdgePix++;
    //     }
    // }
    // int numEdgeV = min((int) (numVertices * edgePortion), numEdgePix);
    // int numNonEdgeV = numVertices - numEdgeV;
    // float edgeP = (float) numEdgeV / numEdgePix;
    // float nonEdgeP = (float) numNonEdgeV / (numPixel - numEdgePix);

    double vertex_start = CycleTimer::currentSeconds();
    vector<Point> vertices = selectVertices(grad, edgeP, nonEdgeP, boundP, edgeThresh, numVertices);
    cout<<"Select Vertices Time: "<< (CycleTimer::currentSeconds() - vertex_start) * 1000 <<"ms"<<endl;

    // write the edge detection result and selected points to img
    // cv::Mat pts = cv::Mat(rows, cols, CV_32F, 0.0);
    // for (int i = 0; i < numVertices; i++)
    // {
    //     int row = vertices[i].y;
    //     int col = vertices[i].x;
    //     pts.at<float>(row, col) = 255.0;
    // }
    // cv::imwrite("points.png", pts);
    // cv::Mat edges = grad * 255.0;
    // cv::imwrite("edges.png", edges);

    return vertices;
}

// Returns a pointer to vertex map (vertexMap[i][j] = (i,j) if it is vertex, (-1, -1) otherwise) on device
void InputFromImageGPU(char* imgPath, int &numVertices, int &rows, int &cols, cv::Mat& img, float edgeP, float nonEdgeP, float boundP, float edgeThresh)
{
    // Read image, set rows and cols
    img = cv::imread(imgPath);
    if (!img.data)
    {
        printf("Error loading image %s\n", imgPath);
        exit(-1);
    }

    total_comp_start = CycleTimer::currentSeconds();
    
    rows = img.rows;
    cols = img.cols;
    int numPixel = rows * cols;
    printf("Img Size: %d x %d\n", rows, cols);
    // scale P to make result visually good
    edgeP *= 2160.f / rows;
    nonEdgeP *= 2160.f / rows;
    boundP *= 2160.f / rows;

    double grad_start = CycleTimer::currentSeconds();
    getGradGPU(img);
    cout<<"Edge Detection Time: "<< (CycleTimer::currentSeconds() - grad_start) * 1000 <<"ms"<<endl;

    double vertex_start = CycleTimer::currentSeconds();
    selectVerticesGPU(edgeThresh, edgeP, nonEdgeP, boundP, rows, cols);
    cout<<"Select Vertices Time: "<< (CycleTimer::currentSeconds() - vertex_start) * 1000 <<"ms"<<endl;
}


int main(int argc, char **argv)
{
    int rows, cols;
    char *imgPath;
    cv::Mat img;
    int numVertices = 0;     // only work in CPU mode
    float edgeP = 4e-3;      // P of selecting a vertex on edge pixel
    float nonEdgeP = 2e-4;   // P of selecting a vertex on non-edge pixel
    float boundP = 0.1;     // P of selecting a vertex on img boundary
    float edgeThresh = 30.0; // threshold for a point being an edge
    bool fromFile = false;

    bool useCPU = false;//Use CPU or GPU
    // parse inputs
    int opt;
    while ((opt = getopt(argc, argv, "f:i:v:e:n:b:t:c")) != -1)
    {
        if(opt == 'c')
            useCPU = true;
        else
        {
            if(opt == 'f')
            {
                fromFile = true;
            }
            else
            {
                switch (opt)
                {
                    case 'i':
                        imgPath = optarg;
                        break;
                    case 'e':
                        edgeP = atof(optarg);
                        break;
                    case 'n':
                        nonEdgeP = atof(optarg);
                        break;
                    case 'b':
                        boundP = atof(optarg);
                        break;
                    case 't':
                        edgeThresh = atof(optarg);
                        break;                        
                    default:
                        cout << "Unrecognized argument: " << char(opt)<<endl;
                }
            }
        }
    }

    if(useCPU)
    {
        cout<<"Using CPU"<<endl;
        double start = CycleTimer::currentSeconds();

        vector<Point> vertices;
        vector<Triangle> triangles;

        // read img, detect edge, select vertices
        if(fromFile)
            vertices = InputFromFile(optarg, numVertices, rows, cols);
        else
            vertices = InputFromImage(imgPath, numVertices, rows, cols, img, edgeP, nonEdgeP, boundP, edgeThresh);

        vector<int> owner(rows * cols, -1);

        double comp_start = CycleTimer::currentSeconds();
        triangles = DelauneyCPU(vertices, owner, rows, cols);
        cout<<"Delaunay Time: "<< (CycleTimer::currentSeconds() - comp_start) * 1000 <<"ms"<<endl;

        // cv::Mat voronoi = drawVoronoi(owner, rows, cols, numVertices);
        // cv::imwrite("voronoi.png", voronoi);

        // cv::Mat triLine = drawTriangleLineOnImg(triangles, voronoi);
        // cv::imwrite("triangle_lines.png", triLine);

        double render_start = CycleTimer::currentSeconds();
        cv::Mat triImg = drawTriangle(triangles, img);
        cout<<"Render Time: "<< (CycleTimer::currentSeconds() - render_start) * 1000 <<"ms"<<endl;

        cout << "Total computation time: " << (CycleTimer::currentSeconds() - total_comp_start) * 1000 << " ms" << endl;
        cv::imwrite("triangle.png", triImg);
        cout << "Total time: " << (CycleTimer::currentSeconds() - start) * 1000 << " ms" << endl;
    }
    else //Use CUDA
    {
        cout<<"Using CUDA"<<endl;
        fakeInit(); // to initialize cuda
        double start = CycleTimer::currentSeconds();

        // read img, detect edge, select vertices
        InputFromImageGPU(imgPath, numVertices, rows, cols, img, edgeP, nonEdgeP, boundP, edgeThresh);

        double comp_start = CycleTimer::currentSeconds();
        DelauneyGPU(rows, cols);
        cout<<"Delaunay Time: "<< (CycleTimer::currentSeconds() - comp_start) * 1000 <<"ms"<<endl;

        double render_start = CycleTimer::currentSeconds();
        cv::Mat triImg = drawTriangleGPU(img);
        cout<<"Render Time: "<< (CycleTimer::currentSeconds() - render_start) * 1000 <<"ms"<<endl;

        cout << "Total computation time: " << (CycleTimer::currentSeconds() - total_comp_start) * 1000 << " ms" << endl;
        cv::imwrite("triangle.png", triImg);
        cout << "Total time: " << (CycleTimer::currentSeconds() - start) * 1000 << " ms" << endl;
    }
    return 0;
}
