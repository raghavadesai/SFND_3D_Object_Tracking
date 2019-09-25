
#include <iostream>
#include <algorithm>
#include <numeric>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "camFusion.hpp"
#include "dataStructures.h"

using namespace std;


// Create groups of Lidar points whose projection into the camera falls into the same bounding box
void clusterLidarWithROI(std::vector<BoundingBox> &boundingBoxes, std::vector<LidarPoint> &lidarPoints, float shrinkFactor, cv::Mat &P_rect_xx, cv::Mat &R_rect_xx, cv::Mat &RT)
{
    // loop over all Lidar points and associate them to a 2D bounding box
    cv::Mat X(4, 1, cv::DataType<double>::type);
    cv::Mat Y(3, 1, cv::DataType<double>::type);

    for (auto it1 = lidarPoints.begin(); it1 != lidarPoints.end(); ++it1)
    {
        // assemble vector for matrix-vector-multiplication
        X.at<double>(0, 0) = it1->x;
        X.at<double>(1, 0) = it1->y;
        X.at<double>(2, 0) = it1->z;
        X.at<double>(3, 0) = 1;

        // project Lidar point into camera
        Y = P_rect_xx * R_rect_xx * RT * X;
        cv::Point pt;
        pt.x = Y.at<double>(0, 0) / Y.at<double>(0, 2); // pixel coordinates
        pt.y = Y.at<double>(1, 0) / Y.at<double>(0, 2);

        vector<vector<BoundingBox>::iterator> enclosingBoxes; // pointers to all bounding boxes which enclose the current Lidar point
        for (vector<BoundingBox>::iterator it2 = boundingBoxes.begin(); it2 != boundingBoxes.end(); ++it2)
        {
            // shrink current bounding box slightly to avoid having too many outlier points around the edges
            cv::Rect smallerBox;
            smallerBox.x = (*it2).roi.x + shrinkFactor * (*it2).roi.width / 2.0;
            smallerBox.y = (*it2).roi.y + shrinkFactor * (*it2).roi.height / 2.0;
            smallerBox.width = (*it2).roi.width * (1 - shrinkFactor);
            smallerBox.height = (*it2).roi.height * (1 - shrinkFactor);

            // check wether point is within current bounding box
            if (smallerBox.contains(pt))
            {
                enclosingBoxes.push_back(it2);
            }

        } // eof loop over all bounding boxes

        // check wether point has been enclosed by one or by multiple boxes
        if (enclosingBoxes.size() == 1)
        { 
            // add Lidar point to bounding box
            enclosingBoxes[0]->lidarPoints.push_back(*it1);
        }

    } // eof loop over all Lidar points
}


void show3DObjects(std::vector<BoundingBox> &boundingBoxes, cv::Size worldSize, cv::Size imageSize, bool bWait)
{
    // create topview image
    cv::Mat topviewImg(imageSize, CV_8UC3, cv::Scalar(255, 255, 255));

    for(auto it1=boundingBoxes.begin(); it1!=boundingBoxes.end(); ++it1)
    {
        // create randomized color for current 3D object
        cv::RNG rng(it1->boxID);
        cv::Scalar currColor = cv::Scalar(rng.uniform(0,150), rng.uniform(0, 150), rng.uniform(0, 150));

        // plot Lidar points into top view image
        int top=1e8, left=1e8, bottom=0.0, right=0.0; 
        float xwmin=1e8, ywmin=1e8, ywmax=-1e8;
        for (auto it2 = it1->lidarPoints.begin(); it2 != it1->lidarPoints.end(); ++it2)
        {
            // world coordinates
            float xw = (*it2).x; // world position in m with x facing forward from sensor
            float yw = (*it2).y; // world position in m with y facing left from sensor
            xwmin = xwmin<xw ? xwmin : xw;
            ywmin = ywmin<yw ? ywmin : yw;
            ywmax = ywmax>yw ? ywmax : yw;

            // top-view coordinates
            int y = (-xw * imageSize.height / worldSize.height) + imageSize.height;
            int x = (-yw * imageSize.width / worldSize.width) + imageSize.width / 2;

            // find enclosing rectangle
            top = top<y ? top : y;
            left = left<x ? left : x;
            bottom = bottom>y ? bottom : y;
            right = right>x ? right : x;

            // draw individual point
            cv::circle(topviewImg, cv::Point(x, y), 4, currColor, -1);
        }

        // draw enclosing rectangle
        cv::rectangle(topviewImg, cv::Point(left, top), cv::Point(right, bottom),cv::Scalar(0,0,0), 2);

        // augment object with some key data
        char str1[200], str2[200];
        sprintf(str1, "id=%d, #pts=%d", it1->boxID, (int)it1->lidarPoints.size());
        putText(topviewImg, str1, cv::Point2f(left-250, bottom+50), cv::FONT_ITALIC, 2, currColor);
        sprintf(str2, "xmin=%2.2f m, yw=%2.2f m", xwmin, ywmax-ywmin);
        putText(topviewImg, str2, cv::Point2f(left-250, bottom+125), cv::FONT_ITALIC, 2, currColor);  
    }

    // plot distance markers
    float lineSpacing = 2.0; // gap between distance markers
    int nMarkers = floor(worldSize.height / lineSpacing);
    for (size_t i = 0; i < nMarkers; ++i)
    {
        int y = (-(i * lineSpacing) * imageSize.height / worldSize.height) + imageSize.height;
        cv::line(topviewImg, cv::Point(0, y), cv::Point(imageSize.width, y), cv::Scalar(255, 0, 0));
    }

    // display image
    string windowName = "3D Objects";
    cv::namedWindow(windowName, 1);
    cv::imshow(windowName, topviewImg);

    if(bWait)
    {
        cv::waitKey(0); // wait for key to be pressed
    }
}

// associate a given bounding box with the keypoints it contains
void clusterKptMatchesWithROI(BoundingBox &boundingBox, std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, std::vector<cv::DMatch> &kptMatches)
{
    std::vector<cv::DMatch> kptMatchesROI;
    for( auto &match : kptMatches)
    {
        if( boundingBox.roi.contains(kptsCurr.at(match.trainIdx).pt))
            kptMatchesROI.push_back(match);
    }
    std::cout << "Found " << kptMatchesROI.size() << " matches with in BB ROI" << std::endl;

    double avgDist = 0.0f;
    if( kptMatchesROI.size() > 0 )
    {
        for( auto itr = kptMatchesROI.begin(); itr != kptMatchesROI.end(); ++itr )
            avgDist += itr->distance;
        
        avgDist = avgDist/ kptMatchesROI.size();
    }
    else
    {
        return;
    }
    /* Do thresholding to elimiante outliers */
    double threshold = avgDist * 0.8;
    for( auto &itr : kptMatchesROI )
    {
        if( itr.distance < threshold )
            boundingBox.kptMatches.push_back(itr);
    }
    std::cout << "After thresholding  " << boundingBox.kptMatches.size() << "matches remained in BB ROI" << std::endl;    

}

double median( vector<double> &distRatios )
{
    double retVal = 0.0;
    size_t size = distRatios.size();
    size_t n    = size/2;

    if( size == 0)
        return retVal;
    
    nth_element(distRatios.begin(), distRatios.begin()+n , distRatios.end() );
    retVal = distRatios[n];
    if( size % 2 == 0 )
        {
        nth_element(distRatios.begin(), distRatios.begin()+n-1 , distRatios.end());
        retVal = ( ( retVal+ distRatios[n-1] ) / 2 );    
        }    
    return retVal;
}

// Compute time-to-collision (TTC) based on keypoint correspondences in successive images
void computeTTCCamera(std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, 
                      std::vector<cv::DMatch> kptMatches, double frameRate, double &TTC, cv::Mat *visImg)
{
    // compute distance ratios between all matched keypoints
    vector<double> distRatios; // stores the distance ratios for all keypoints between curr. and prev. frame

    if (kptMatches.size() == 0)
    {
        TTC = NAN;
        return;
    }
    
    for (auto it1 = kptMatches.begin(); it1 != kptMatches.end() - 1; ++it1)
    { // outer kpt. loop

        // get current keypoint and its matched partner in the prev. frame
        cv::KeyPoint kpOuterCurr = kptsCurr.at(it1->trainIdx);
        cv::KeyPoint kpOuterPrev = kptsPrev.at(it1->queryIdx);

        for (auto it2 = kptMatches.begin() + 1; it2 != kptMatches.end(); ++it2)
        { // inner kpt.-loop

            double minDist = 100.0; // min. required distance

            // get next keypoint and its matched partner in the prev. frame
            cv::KeyPoint kpInnerCurr = kptsCurr.at(it2->trainIdx);
            cv::KeyPoint kpInnerPrev = kptsPrev.at(it2->queryIdx);

            // compute distances and distance ratios
            double distCurr = cv::norm(kpOuterCurr.pt - kpInnerCurr.pt);
            double distPrev = cv::norm(kpOuterPrev.pt - kpInnerPrev.pt);

            if (distPrev > std::numeric_limits<double>::epsilon() && distCurr >= minDist)
            { // avoid division by zero

                double distRatio = distCurr / distPrev;
                distRatios.push_back(distRatio);
            }
        } // eof inner loop over all matched kpts
    }     // eof outer loop over all matched kpts

    // only continue if list of distance ratios is not empty
    if (distRatios.size() == 0)
    {
        TTC = NAN;
        return;
    }

    double dT               = 1.0f / frameRate;
    double medianDistRatios = median(distRatios);

    TTC = -dT / (1 - medianDistRatios);
}


void computeTTCLidar(std::vector<LidarPoint> &lidarPointsPrev,
                     std::vector<LidarPoint> &lidarPointsCurr, double frameRate, double &TTC)
{
    // auxiliary variables
        double dT = 1.0f/frameRate;        // time between two measurements in seconds
        double laneWidth = 4.0; // assumed width of the ego lane

        // find closest distance to Lidar points within ego lane
        std::vector<double> prev_x, curr_x;
        double avgXPrev, avgXCurr;

        for (auto it = lidarPointsPrev.begin(); it != lidarPointsPrev.end(); ++it)
        {
            if( abs(it->y) <= laneWidth/2.0 )
                prev_x.push_back(it->x);
        }

        for (auto it = lidarPointsCurr.begin(); it != lidarPointsCurr.end(); ++it)
        {
            if (abs(it->y) <= laneWidth / 2.0)
                curr_x.push_back(it->x);
        }

        if( prev_x.size() > 0 && curr_x.size() > 0)
        {
            avgXPrev = std::accumulate(prev_x.begin(), prev_x.end(), 0.0) / prev_x.size();            
            avgXCurr = std::accumulate(curr_x.begin(), curr_x.end(), 0.0) / curr_x.size();
        }
        else
        {
            TTC = NAN;
            return;
        }       

        // compute TTC from both measurements
        TTC = avgXCurr * dT / (avgXPrev - avgXCurr);
}


void matchBoundingBoxes(std::vector<cv::DMatch> &matches, std::map<int, int> &bbBestMatches, DataFrame &prevFrame, DataFrame &currFrame)
{
    int prev_bb_cnt = prevFrame.boundingBoxes.size();
    int curr_bb_cnt = currFrame.boundingBoxes.size();
    int point_count[prev_bb_cnt][curr_bb_cnt] = {0};

    cout << "Started Point Coutning per BB" << endl;

    for( auto itr = matches.begin(); itr != matches.end(); ++itr )
    {
        cv::KeyPoint query = prevFrame.keypoints[itr->queryIdx];
        auto queryPt = cv::Point(query.pt.x, query.pt.y);
        bool queryPtFound = false;

        cv::KeyPoint train = currFrame.keypoints[itr->trainIdx];
        auto trainPt = cv::Point(train.pt.x, train.pt.y);
        bool trainPtFound = false;

        std::vector<int> queryId, trainId;

        for( int i = 0; i < prev_bb_cnt; i++ )
        {
            if( prevFrame.boundingBoxes[i].roi.contains(queryPt))
            {
                queryPtFound = true;
                queryId.push_back(i);
            }
        }

        for( int i = 0; i < prev_bb_cnt; i++ )
        {
            if( currFrame.boundingBoxes[i].roi.contains(trainPt))
            {
                trainPtFound = true;
                trainId.push_back(i);
            }
        }

        if( queryPtFound && trainPtFound )
        {
            for ( auto a_id_prev: queryId)
               for ( auto a_id_curr: trainId)
                   point_count[a_id_prev][a_id_curr] += 1;                  

        }
    } //end loop over all matching Points

    cout << "Done with Point Coutning per BB" << endl;
    /*Pick the bestmatch for each bb */
    for( int i =0; i< prev_bb_cnt; i++ )
    {
        int max_count = 0;
        int id_max = 0;
        for( int j = 0; j<curr_bb_cnt; j++)
        {
            if( point_count[i][j] > max_count )
            {
            max_count = point_count[i][j];
            id_max = j;
            }
        }
        bbBestMatches[i] = id_max;
    }
    bool bMsg = true;
    if (bMsg)
        for (int i = 0; i < prev_bb_cnt; i++)
             cout << "Box " << i << " matches " << bbBestMatches[i]<< " box" << endl;
    
}
