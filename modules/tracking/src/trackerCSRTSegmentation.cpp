// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

#include "precomp.hpp"

#include "trackerCSRTSegmentation.hpp"

#include <fstream>
#include <iostream>
#include <vector>
#include <iostream>

//-------------------- HISTOGRAM CLASS --------------------
namespace cv
{

Histogram::Histogram(int numBins)
{
    m_numBins = numBins;
    p_coef = 1;
    p_bins.resize(m_numBins, 0);
 
}

void Histogram::extractForegroundHistogram(const cv::Mat & img,
        cv::Mat weights, bool useMatWeights, int x1, int y1, int x2, int y2)
{
    CV_Assert(img.channels() == 1);
    if (!useMatWeights){
        //weights are epanechnikov distr. with peek at the center of the image;
        double cx = x1 + (x2-x1)/2.;
        double cy = y1 + (y2-y1)/2.;
        double kernelSize_width = 1.0/(0.5*static_cast<double>(x2-x1)*1.4142+1);  //sqrt(2)
        double kernelSize_height = 1.0/(0.5*static_cast<double>(y2-y1)*1.4142+1);

        cv::Mat kernelWeight(img.rows, img.cols, CV_64FC1);
        for (int y = y1; y < y2+1; ++y){
            double * weightPtr = kernelWeight.ptr<double>(y);
            double tmp_y = std::pow((cy-y)*kernelSize_height, 2);
            for (int x = x1; x < x2+1; ++x){
                weightPtr[x] = kernelProfile_Epanechnikov(std::pow((cx-x)*kernelSize_width,2) + tmp_y);
            }
        }
        weights = kernelWeight;
    }
    //extract pixel values and compute histogram
    double rangePerBinInverse = static_cast<double>(m_numBins)/256.0;  // 1 / (imgRange/numBinsPerDim)
    double sum = 0;
    for (int y = y1; y < y2+1; ++y){
        const uchar * dataPtr = img.ptr<uchar>(y);
        const double * weightPtr = weights.ptr<double>(y);

        for (int x = x1; x < x2+1; ++x){
            int id = p_coef*cvFloor(rangePerBinInverse*dataPtr[x]);
            p_bins[id] += weightPtr[x];
            sum += weightPtr[x];
        }
    }
    //normalize
    sum = 1./sum;
    for(int i = 0; i < m_numBins; ++i)
        p_bins[i] *= sum;
   
}

void Histogram::extractBackGroundHistogram(
        const cv::Mat& img,
        int x1, int y1, int x2, int y2,
        int outer_x1, int outer_y1, int outer_x2, int outer_y2)
{
    CV_Assert(img.channels() == 1);
    //extract pixel values and compute histogram
    double rangePerBinInverse = static_cast<double>(m_numBins)/256.0;  // 1 / (imgRange/numBinsPerDim)
    double sum = 0;
    for (int y = outer_y1; y < outer_y2; ++y){

        const uchar * dataPtr = img.ptr<uchar>(y);

        for (int x = outer_x1; x < outer_x2; ++x){
            if (x >= x1 && x <= x2 && y >= y1 && y <= y2)
                continue;

            int id = p_coef * cvFloor(rangePerBinInverse * dataPtr[x]);
            p_bins[id] += 1.0;
            sum += 1.0;
        }
    }
    //normalize
    sum = 1./sum;
    for(int i = 0; i < m_numBins; ++i)
        p_bins[i] *= sum;
}

cv::Mat Histogram::backProject(cv::Mat& img)
{
    CV_Assert(img.channels() == 1);
    cv::Mat backProject(img.rows, img.cols, CV_64FC1);
    double rangePerBinInverse = static_cast<double>(m_numBins)/256.0;  // 1 / (imgRange/numBinsPerDim)

    for (int y = 0; y < img.rows; ++y){
        double * backProjectPtr = backProject.ptr<double>(y);
        const uchar *dataPtr = img.ptr<uchar>(y);

        for (int x = 0; x < img.cols; ++x){
            int id = p_coef*cvFloor(rangePerBinInverse*dataPtr[x]);
            backProjectPtr[x] = p_bins[id];
        }
    }
    return backProject;
}

// add new methods
std::vector<double> Histogram::getHistogramVector() {
    return p_bins;
}

void Histogram::setHistogramVector(double *vector) {
    for (size_t i=0; i<p_bins.size(); i++) {
        p_bins[i] = vector[i];
    }
}

//-------------------- SEGMENT CLASS --------------------
std::pair<cv::Mat, cv::Mat> Segment::computePosteriors(
        cv::Mat& img,
        int x1, int y1, int x2, int y2,
        cv::Mat weights, cv::Mat fgPrior, cv::Mat bgPrior,
        const Histogram &fgHistPrior, int numBins)
{
    //preprocess and normalize all data
    CV_Assert(img.channels() == 1);

    //fit target to the image
    x1 = std::min(std::max(x1, 0), img.cols-1);
    y1 = std::min(std::max(y1, 0), img.rows-1);
    x2 = std::max(std::min(x2, img.cols-1), 0);
    y2 = std::max(std::min(y2, img.rows-1), 0);

    //enlarge bbox by 1/3 of its size for background area
    int offsetX = (x2-x1)/3;
    int offsetY = (y2-y1)/3;
    int outer_y1 = std::max(0, (int)(y1-offsetY));
    int outer_y2 = std::min(img.rows, (int)(y2+offsetY+1));
    int outer_x1 = std::max(0, (int)(x1-offsetX));
    int outer_x2 = std::min(img.cols, (int)(x2+offsetX+1));

    //extract histogram from original data -> more pixels better representation of distr. by histograms
    Histogram hist_target =
        (fgHistPrior.m_numBins == numBins)
        ? fgHistPrior : Histogram(numBins);
    Histogram hist_background(numBins);
    if (weights.cols == 0)
        hist_target.extractForegroundHistogram(img, cv::Mat(), false, x1, y1, x2, y2);
    else
        hist_target.extractForegroundHistogram(img, weights, true, x1, y1, x2, y2);
    hist_background.extractBackGroundHistogram(img, x1, y1, x2, y2,
            outer_x1, outer_y1, outer_x2, outer_y2);

    //compute resize factor so that the max area is 1000 (=avg. size ~ 32x32)
    double factor = sqrt(1000.0/((x2-x1)*(y2-y1)));
    if (factor > 1)
        factor = 1.0;
    cv::Size newSize(cvFloor((x2-x1)*factor), cvFloor((y2-y1)*factor));

    //rescale input data
    cv::Rect roiRect_inner = cv::Rect(x1, y1, x2-x1, y2-y1);
    cv::Mat imgROI_inner;
    cv::resize(img(roiRect_inner), imgROI_inner, newSize);

    //initialize priors if there is no external source and rescale
    cv::Mat fgPriorScaled;
    if (fgPrior.cols == 0)
        fgPriorScaled = 0.5*cv::Mat::ones(newSize, CV_64FC1);
    else
        cv::resize(fgPrior(roiRect_inner), fgPriorScaled, newSize);
    cv::Mat bgPriorScaled;
    if (bgPrior.cols == 0)
        bgPriorScaled = 0.5*cv::Mat::ones(newSize, CV_64FC1);
    else
        cv::resize(bgPrior(roiRect_inner), bgPriorScaled, newSize);

    //backproject pixels likelihood
    cv::Mat foregroundLikelihood = hist_target.backProject(imgROI_inner).mul(fgPriorScaled);
    cv::Mat backgroundLikelihood = hist_background.backProject(imgROI_inner).mul(bgPriorScaled);

    double p_b = std::sqrt((std::pow(outer_x2-outer_x1, 2) + std::pow(outer_y2-outer_y1, 2)) /
            (std::pow(x2-x1, 2) + std::pow(y2-y1, 2))) ;
    double p_o = 1./(p_b + 1);

    //convert likelihoods to posterior prob. (Bayes rule)
    cv::Mat prob_o(newSize, foregroundLikelihood.type());
    prob_o = p_o*foregroundLikelihood / (p_o*foregroundLikelihood + p_b*backgroundLikelihood);
    cv::Mat prob_b = 1.0 - prob_o;

    std::pair<cv::Mat, cv::Mat> sizedProbs = getRegularizedSegmentation(prob_o, prob_b, fgPriorScaled, bgPriorScaled);

    //resize probs to original size
    std::pair<cv::Mat, cv::Mat> probs;
    cv::resize(sizedProbs.first, probs.first, cv::Size(roiRect_inner.width, roiRect_inner.height));
    cv::resize(sizedProbs.second, probs.second, cv::Size(roiRect_inner.width, roiRect_inner.height));

    return probs;
}

std::pair<cv::Mat, cv::Mat> Segment::computePosteriors2(
    cv::Mat &img, int x1, int y1, int x2, int y2, double p_b,
    cv::Mat fgPrior, cv::Mat bgPrior, Histogram hist_target, Histogram hist_background)
{
    //preprocess and normalize all data
     CV_Assert(img.channels() == 1);

    //fit target to the image
     x1 = std::min(std::max(x1, 0), img.cols - 1);
     y1 = std::min(std::max(y1, 0), img.rows - 1);
     x2 = std::max(std::min(x2, img.cols - 1), 0);
     y2 = std::max(std::min(y2, img.rows - 1), 0);

    // calculate width and height of the region
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    w = std::min(std::max(w, 1), img.cols);
    h = std::min(std::max(h, 1), img.rows);

    //double p_o = 1./(p_b + 1);
    double p_o = 1. - p_b;

    //compute resize factor so that the max area is 1000 (=avg. size ~ 32x32)
    double factor = sqrt(1000.0/(w*h));
    if (factor > 1)
        factor = 1.0;
    cv::Size newSize(cvFloor(w*factor), cvFloor(h*factor));

    //rescale input data
    cv::Rect roiRect_inner = cv::Rect(x1, y1, w, h);
    cv::Mat imgROI_inner;
    cv::resize(img(roiRect_inner), imgROI_inner, newSize);

    //initialize priors if there is no external source and rescale
    cv::Mat fgPriorScaled;
    if (fgPrior.cols == 0)
        fgPriorScaled = 0.5*cv::Mat::ones(newSize, CV_64FC1);
    else
        cv::resize(fgPrior(roiRect_inner), fgPriorScaled, newSize);
    cv::Mat bgPriorScaled;
    if (bgPrior.cols == 0)
        bgPriorScaled = 0.5*cv::Mat::ones(newSize, CV_64FC1);
    else
        cv::resize(bgPrior(roiRect_inner), bgPriorScaled, newSize);

    //backproject pixels likelihood
    cv::Mat foregroundLikelihood = hist_target.backProject(imgROI_inner).mul(fgPriorScaled);
    cv::Mat backgroundLikelihood = hist_background.backProject(imgROI_inner).mul(bgPriorScaled);

    //convert likelihoods to posterior prob. (Bayes rule)
    cv::Mat prob_o(newSize, foregroundLikelihood.type());
    prob_o = p_o*foregroundLikelihood / (p_o*foregroundLikelihood + p_b*backgroundLikelihood);
    cv::Mat prob_b = 1.0 - prob_o;

    std::pair<cv::Mat, cv::Mat> sizedProbs = getRegularizedSegmentation(prob_o, prob_b,
            fgPriorScaled, bgPriorScaled);
    //std::pair<cv::Mat, cv::Mat> sizedProbs = std::pair<cv::Mat, cv::Mat>(prob_o, prob_b);

    //resize probs to original size
    std::pair<cv::Mat, cv::Mat> probs;
    cv::resize(sizedProbs.first, probs.first, cv::Size(roiRect_inner.width, roiRect_inner.height));
    cv::resize(sizedProbs.second, probs.second, cv::Size(roiRect_inner.width, roiRect_inner.height));

    return probs;
}

std::pair<cv::Mat, cv::Mat> Segment::computePosteriors2(cv::Mat &img,
        cv::Mat fgPrior, cv::Mat bgPrior, Histogram hist_target, Histogram hist_background)
{
    //preprocess and normalize all data
    CV_Assert(img.channels() == 1);

    //fit target to the image
    int x1 = 0;
    int y1 = 0;
    int x2 = img.cols-1;
    int y2 = img.rows-1;

    //compute resize factor so that we control the max area ~32^2
    double factor = sqrt(1000./((x2-x1)*(y2-y1)));
    //double factor = 1;
    if (factor > 1)
        factor = 1.0;
    cv::Size newSize(cvFloor((x2-x1)*factor), cvFloor((y2-y1)*factor));

    //rescale input data
    cv::Rect roiRect_inner = cv::Rect(x1, y1, x2-x1+1, y2-y1+1);
    cv::Mat imgROI_inner;
    cv::resize(img(roiRect_inner), imgROI_inner, newSize);

    //initialize priors if there is no external source and rescale
    cv::Mat fgPriorScaled;
    if (fgPrior.cols == 0)
        fgPriorScaled = 0.5*cv::Mat::ones(newSize, CV_64FC1);
    else
        cv::resize(fgPrior(roiRect_inner), fgPriorScaled, newSize);

    cv::Mat bgPriorScaled;
    if (bgPrior.cols == 0)
        bgPriorScaled = 0.5*cv::Mat::ones(newSize, CV_64FC1);
    else
        cv::resize(bgPrior(roiRect_inner), bgPriorScaled, newSize);

    //backproject pixels likelihood
    cv::Mat foregroundLikelihood = hist_target.backProject(imgROI_inner).mul(fgPriorScaled);
    cv::Mat backgroundLikelihood = hist_background.backProject(imgROI_inner).mul(bgPriorScaled);

    //prior for posterior, relative to the number of pixels in bg and fg
    double p_b = 5./3.;
    double p_o = 1./(p_b + 1);

    //convert likelihoods to posterior prob. (Bayes rule)
    cv::Mat prob_o(newSize, foregroundLikelihood.type());
    prob_o = p_o*foregroundLikelihood / (p_o*foregroundLikelihood + p_b*backgroundLikelihood);
    cv::Mat prob_b = 1.0 - prob_o;

    std::pair<cv::Mat, cv::Mat> sizedProbs = getRegularizedSegmentation(prob_o, prob_b, fgPriorScaled, bgPriorScaled);

    //resize probs to original size
    std::pair<cv::Mat, cv::Mat> probs;
    cv::resize(sizedProbs.first, probs.first, cv::Size(roiRect_inner.width, roiRect_inner.height));
    cv::resize(sizedProbs.second, probs.second, cv::Size(roiRect_inner.width, roiRect_inner.height));

    return probs;
}

std::pair<cv::Mat, cv::Mat> Segment::getRegularizedSegmentation(
        cv::Mat &prob_o, cv::Mat &prob_b, cv::Mat & prior_o, cv::Mat & prior_b)
{
    int hsize = cvFloor(std::max(1.0, (double)cvFloor(static_cast<double>(prob_b.cols)*3./50. + 0.5)));
    int lambdaSize = hsize*2+1;

    //compute gaussian kernel
    cv::Mat lambda(lambdaSize, lambdaSize, CV_64FC1);
    double std2 = std::pow(hsize/3.0, 2);
    double sumLambda = 0.0;
    for (int y = -hsize; y < hsize + 1; ++y){
        double * lambdaPtr = lambda.ptr<double>(y+hsize);
        double tmp_y = y*y;
        for (int x = -hsize; x < hsize +1; ++x){
            double tmp_gauss = gaussian(x*x, tmp_y, std2);
            lambdaPtr[x+hsize] = tmp_gauss;
            sumLambda += tmp_gauss;
        }
    }
    sumLambda -= lambda.at<double>(hsize, hsize);
    //set center of kernel to 0
    lambda.at<double>(hsize, hsize) = 0.0;
    sumLambda = 1.0/sumLambda;
    //normalize kernel to sum to 1
    lambda = lambda*sumLambda;

    //create lambda2 kernel
    cv::Mat lambda2 = lambda.clone();
    lambda2.at<double>(hsize, hsize) = 1.0;

    double terminateThr = 1e-1;
    double logLike = std::numeric_limits<double>::max();
    int maxIter = 50;

    //return values
    cv::Mat Qsum_o(prior_o.rows, prior_o.cols, prior_o.type());
    cv::Mat Qsum_b(prior_o.rows, prior_o.cols, prior_o.type());

    //algorithm temporal
    cv::Mat Si_o(prior_o.rows, prior_o.cols, prior_o.type());
    cv::Mat Si_b(prior_o.rows, prior_o.cols, prior_o.type());
    cv::Mat Ssum_o(prior_o.rows, prior_o.cols, prior_o.type());
    cv::Mat Ssum_b(prior_o.rows, prior_o.cols, prior_o.type());
    cv::Mat Qi_o(prior_o.rows, prior_o.cols, prior_o.type());
    cv::Mat Qi_b(prior_o.rows, prior_o.cols, prior_o.type());
    cv::Mat logQo(prior_o.rows, prior_o.cols, prior_o.type());
    cv::Mat logQb(prior_o.rows, prior_o.cols, prior_o.type());

    int i;
    for (i = 0; i < maxIter; ++i){
        //follows the equations from Kristan et al. ACCV2014 paper
        //"A graphical model for rapid obstacle image-map estimation from unmanned surface vehicles"
        cv::Mat P_Io = prior_o.mul(prob_o) + std::numeric_limits<double>::epsilon();
        cv::Mat P_Ib = prior_b.mul(prob_b) + std::numeric_limits<double>::epsilon();

        cv::filter2D(prior_o, Si_o, -1, lambda, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);
        cv::filter2D(prior_b, Si_b, -1, lambda, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);
        Si_o = Si_o.mul(prior_o);
        Si_b = Si_b.mul(prior_b);
        cv::Mat normSi = 1.0/(Si_o + Si_b);
        Si_o = Si_o.mul(normSi);
        Si_b = Si_b.mul(normSi);
        cv::filter2D(Si_o, Ssum_o, -1, lambda2, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);
        cv::filter2D(Si_b, Ssum_b, -1, lambda2, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);

        cv::filter2D(P_Io, Qi_o, -1, lambda, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);
        cv::filter2D(P_Ib, Qi_b, -1, lambda, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);
        Qi_o = Qi_o.mul(P_Io);
        Qi_b = Qi_b.mul(P_Ib);
        cv::Mat normQi = 1.0/(Qi_o + Qi_b);
        Qi_o = Qi_o.mul(normQi);
        Qi_b = Qi_b.mul(normQi);
        cv::filter2D(Qi_o, Qsum_o, -1, lambda2, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);
        cv::filter2D(Qi_b, Qsum_b, -1, lambda2, cv::Point(-1, -1), 0, cv::BORDER_REFLECT);

        prior_o = (Qsum_o + Ssum_o)*0.25;
        prior_b = (Qsum_b + Ssum_b)*0.25;
        cv::Mat normPI = 1.0/(prior_o + prior_b);
        prior_o = prior_o.mul(normPI);
        prior_b = prior_b.mul(normPI);

        //converge ?
        cv::log(Qsum_o, logQo);
        cv::log(Qsum_b, logQb);
        cv::Scalar mean = cv::sum(logQo+logQb);
        double logLikeNew = -mean.val[0]/(2*Qsum_o.rows*Qsum_o.cols);
        if (std::abs(logLike - logLikeNew) < terminateThr)
            break;
        logLike = logLikeNew;
    }
    return std::pair<cv::Mat, cv::Mat>(Qsum_o, Qsum_b);
}

} //cv namespace
//---------------------------------------------------------------------------------------------------------------------
