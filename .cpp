// CMOSSETrackerAppView.cpp : implementation of the CMOSSETrackerAppView class

#include "stdafx.h"
#include <atlconv.h>
#ifndef SHARED_HANDLERS
#include "MOSSETrackerApp.h"
#endif

#include "MOSSETrackerAppDoc.h"
#include "MOSSETrackerAppView.h"

#include <fstream>  // for CSV logging
#include <deque>    // for graph buffering

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// Global log and buffer for confidence graph
std::ofstream logFile("tracking_log.csv");
std::deque<float> confidenceHistory;

IMPLEMENT_DYNCREATE(CMOSSETrackerAppView, CView)

BEGIN_MESSAGE_MAP(CMOSSETrackerAppView, CView)
	ON_COMMAND(ID_FILE_PRINT, &CView::OnFilePrint)
	ON_COMMAND(ID_FILE_PRINT_DIRECT, &CView::OnFilePrint)
	ON_COMMAND(ID_FILE_PRINT_PREVIEW, &CMOSSETrackerAppView::OnFilePrintPreview)
	ON_COMMAND(ID_INPUT_WEBCAM, &CMOSSETrackerAppView::OnInputWebcam)
	ON_COMMAND(ID_TRACKING_ENABLESAVE, &CMOSSETrackerAppView::OnTrackingEnableSave)
	ON_COMMAND(ID_FILE_OPENVIDEO, &CMOSSETrackerAppView::OpenVideoFile)
	ON_COMMAND(ID_TRACKING_PAUSE_RESUME, &CMOSSETrackerAppView::OnTrackingPauseResume)
	ON_WM_CONTEXTMENU()
	ON_WM_RBUTTONUP()
	ON_WM_TIMER()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_MOUSEMOVE()
	ON_WM_KEYDOWN()
END_MESSAGE_MAP()

CMOSSETrackerAppView::CMOSSETrackerAppView() noexcept
	: learning_rate(0.125f), frame_count(0), filter_initialized(false) {}
CMOSSETrackerAppView::~CMOSSETrackerAppView() {
	if (logFile.is_open()) logFile.close();
}

BOOL CMOSSETrackerAppView::PreCreateWindow(CREATESTRUCT& cs)
{
	cs.style |= WS_TABSTOP;
	return CView::PreCreateWindow(cs);
}

void CMOSSETrackerAppView::OnDraw(CDC* pDC)
{
	if (!currentFrame.empty()) {
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = currentFrame.cols;
		bmi.bmiHeader.biHeight = -currentFrame.rows;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		cv::Mat display;
		if (currentFrame.channels() == 1)
			cv::cvtColor(currentFrame, display, cv::COLOR_GRAY2BGR);
		else
			display = currentFrame;

		StretchDIBits(pDC->GetSafeHdc(), 0, 0, display.cols, display.rows,
			0, 0, display.cols, display.rows,
			display.data, &bmi, DIB_RGB_COLORS, SRCCOPY);
	}

	if (selecting && selectionRect.Width() > 0 && selectionRect.Height() > 0) {
		CBrush* pOldBrush = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
		CPen pen(PS_SOLID, 2, RGB(255, 0, 0));
		CPen* pOldPen = pDC->SelectObject(&pen);
		pDC->Rectangle(&selectionRect);
		pDC->SelectObject(pOldPen);
		pDC->SelectObject(pOldBrush);
	}

	if (tracking && roiSelected) {
		CString debug;
		debug.Format(_T("ROI: (%d,%d) %dx%d"), roi.x, roi.y, roi.width, roi.height);
pDC->TextOut(10, 10, debug);

		// Draw center point
		CPoint center(roi.x + roi.width / 2, roi.y + roi.height / 2);
		pDC->Ellipse(center.x - 3, center.y - 3, center.x + 3, center.y + 3);

		CString fpsText;
		fpsText.Format(_T("FPS: %.1f"), fps);
		pDC->TextOut(10, 30, fpsText);
	}

}

void CMOSSETrackerAppView::OnInitialUpdate()
{
	CView::OnInitialUpdate();
	cap.open(0);
	if (!cap.isOpened()) {
		AfxMessageBox(_T("Failed to open webcam."));
		return;
	}

	if (enableSaving) {
		saveFilename = _T("output.avi");
		cv::Size size = cv::Size((int)cap.get(cv::CAP_PROP_FRAME_WIDTH),
			(int)cap.get(cv::CAP_PROP_FRAME_HEIGHT));
		double fps_cap = cap.get(cv::CAP_PROP_FPS);
		if (fps_cap == 0) fps_cap = 30.0;

		cv::String cvFilename = std::string(CT2A(saveFilename));
		videoWriter.open(cvFilename, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30, frame.size(), true);
		if (!videoWriter.isOpened()) {
			AfxMessageBox(_T("Failed to initialize video writer."));
			enableSaving = false;
		}
	}

	KillTimer(nTimer);
	SetTimer(nTimer, 33, NULL);
}

void CMOSSETrackerAppView::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == nTimer && cap.isOpened()) {
		if (!paused) {
			cap >> frame;
			if (frame.empty()) return;
			frame.copyTo(currentFrame);
			cvtColor(currentFrame, gray, COLOR_BGR2GRAY);

			auto now = std::chrono::steady_clock::now();
			static auto prev = now;
			float elapsed = std::chrono::duration<float>(now - prev).count();
			prev = now;
			if (elapsed > 0)
				fps = 1.0f / elapsed;
		}
		if (tracking && roiSelected && roi.width > 0 && roi.height > 0) {
			updateMOSSE();
			rectangle(currentFrame, roi, Scalar(0, 255, 0), 2);
		}
		else if (roiSelected && roi.width > 0 && roi.height > 0) {
			rectangle(currentFrame, roi, Scalar(255, 0, 0), 2);
		}

		if (enableSaving && videoWriter.isOpened()) {
			videoWriter.write(currentFrame);
		}

		Invalidate(FALSE);
	}
	CView::OnTimer(nIDEvent);
}

void CMOSSETrackerAppView::initMOSSE()
{
	if (currentFrame.empty() || roi.width <= 0 || roi.height <= 0) return;

	// Extract and preprocess the initial patch
	Mat grayFrame;
	cvtColor(currentFrame, grayFrame, COLOR_BGR2GRAY);

	// Ensure ROI is within bounds
	roi.x = max(0, min(roi.x, grayFrame.cols - roi.width));
	roi.y = max(0, min(roi.y, grayFrame.rows - roi.height));

	Mat patch = grayFrame(roi).clone();
	Mat processed_patch = preprocessPatch(patch);

	// Create Gaussian target
	Mat target = createGaussianTarget(patch.size(), 2.0f);

	// Initialize filter
	H_num = Mat::zeros(patch.size(), CV_32FC2);
	H_den = Mat::zeros(patch.size(), CV_32FC2);

	// Train initial filter
	updateFilter(processed_patch, target);
	filter_initialized = true;
	frame_count = 0;

	CString msg;
	msg.Format(_T("MOSSE initialized at (%d,%d) size %dx%d"),
		roi.x, roi.y, roi.width, roi.height);
	OutputDebugString(msg);
}

Mat CMOSSETrackerAppView::preprocessPatch(const Mat& patch)
{
	Mat result;
	patch.convertTo(result, CV_32F);

	// Normalize to zero mean, unit variance
	Scalar mean, stddev;
	meanStdDev(result, mean, stddev);
	result = (result - mean[0]) / (stddev[0] + 1e-5);

	// Apply cosine window
	if (cos_window.size() != patch.size()) {
		createHanningWindow(cos_window, patch.size(), CV_32F);
	}
	result = result.mul(cos_window);

	return result;
}

Mat CMOSSETrackerAppView::createGaussianTarget(Size sz, float sigma)
{
	Mat target = Mat::zeros(sz, CV_32F);
	int cx = sz.width / 2;
	int cy = sz.height / 2;

	float sigma_sq = sigma * sigma;

	for (int y = 0; y < sz.height; y++) {
		for (int x = 0; x < sz.width; x++) {
			int dx = x - cx;
			int dy = y - cy;
			target.at<float>(y, x) = exp(-(dx*dx + dy*dy) / (2.0f * sigma_sq));
		}
	}

	return target;
}

void CMOSSETrackerAppView::updateFilter(const Mat& patch, const Mat& target)
{
	Mat F_patch, F_target;
	dft(patch, F_patch, DFT_COMPLEX_OUTPUT);
	dft(target, F_target, DFT_COMPLEX_OUTPUT);

	Mat new_H_num, new_H_den;
	mulSpectrums(F_target, F_patch, new_H_num, 0, true);  // target * conj(patch)
	mulSpectrums(F_patch, F_patch, new_H_den, 0, true);   // patch * conj(patch)

	if (frame_count == 0) {
		// First frame - initialize
		H_num = new_H_num.clone();
		H_den = new_H_den.clone();
	}
	else {
		// Update with learning rate
		H_num = (1.0f - learning_rate) * H_num + learning_rate * new_H_num;
		H_den = (1.0f - learning_rate) * H_den + learning_rate * new_H_den;
	}

	frame_count++;
}


void CMOSSETrackerAppView::updateMOSSE()
{
	if (!filter_initialized || gray.empty()) return;

	// Check bounds
	if (roi.x < 0 || roi.y < 0 ||
		roi.x + roi.width > gray.cols ||
		roi.y + roi.height > gray.rows) {
		return;
	}

	// Extract current patch
	Mat current_patch = gray(roi).clone();
	Mat processed_patch = preprocessPatch(current_patch);

	// Compute response
	Mat F_patch, response_f, response;
	dft(processed_patch, F_patch, DFT_COMPLEX_OUTPUT);

	// Add regularization to denominator
	Mat H_den_reg = H_den + Scalar::all(0.01f);
	Mat H_filter;
	divide(H_num, H_den_reg, H_filter);

	mulSpectrums(H_filter, F_patch, response_f, 0, false);
	idft(response_f, response, DFT_SCALE | DFT_REAL_OUTPUT);

	// Find peak
	Point maxLoc;
	double maxVal;
	minMaxLoc(response, NULL, &maxVal, NULL, &maxLoc);

	// Convert to displacement from center
	int dx = maxLoc.x - roi.width / 2;
	int dy = maxLoc.y - roi.height / 2;

	// Handle wraparound (FFT property)
	if (dx > roi.width / 2) dx -= roi.width;
	if (dy > roi.height / 2) dy -= roi.height;
	if (dx < -roi.width / 2) dx += roi.width;
	if (dy < -roi.height / 2) dy += roi.height;

	// Apply displacement with confidence-based damping
	float confidence = (float)maxVal;
	float damping = min(0.8f, max(0.1f, confidence));

	int new_x = roi.x + (int)(damping * dx);
	int new_y = roi.y + (int)(damping * dy);

	// Clamp to image bounds
	new_x = max(0, min(new_x, gray.cols - roi.width));
	new_y = max(0, min(new_y, gray.rows - roi.height));

	// Update ROI
	roi.x = new_x;
	roi.y = new_y;

	// Update filter if confidence is high enough
	if (confidence > 0.1f) {
		Mat target = createGaussianTarget(current_patch.size(), 2.0f);
		updateFilter(processed_patch, target);
	}

	if (frame_count == 0) {
		trackingStartTime = std::chrono::steady_clock::now();
	}
	auto now = std::chrono::steady_clock::now();
	int timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - trackingStartTime).count();

	confidence_values.push_back(confidence);
	timestamps_ms.push_back(timestamp_ms);

	CString debug;
	debug.Format(_T("Frame %d | Confidence: %.4f\n"), frame_count, confidence);
	OutputDebugString(debug);
}



void CMOSSETrackerAppView::OpenVideoFile()
{
	CFileDialog dlg(TRUE, _T(".mp4"), NULL, OFN_FILEMUSTEXIST, _T("Video Files (.mp4;.avi)|.mp4;.avi|All Files (.)|.*||"));
	if (dlg.DoModal() == IDOK) {
		if (cap.isOpened()) cap.release();
		CString path = dlg.GetPathName();
		cv::String filename = CT2A(path);
		cap.open(filename);
		if (!cap.isOpened()) {
			AfxMessageBox(_T("Failed to open video file."));
			return;
		}

		if (enableSaving) {
			saveFilename = _T("output.avi");
			cv::Size size = cv::Size((int)cap.get(cv::CAP_PROP_FRAME_WIDTH),
				(int)cap.get(cv::CAP_PROP_FRAME_HEIGHT));
			double fps_cap = cap.get(cv::CAP_PROP_FPS);
			if (fps_cap == 0) fps_cap = 30.0;

			cv::String cvFilename = std::string(CT2A(saveFilename));
			videoWriter.open(cvFilename, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30, frame.size(), true);
			if (!videoWriter.isOpened()) {
				AfxMessageBox(_T("Failed to initialize video writer."));
				enableSaving = false;
			}
		}

		tracking = false;
		roiSelected = false;
		KillTimer(nTimer);
		SetTimer(nTimer, 33, NULL);
	}
}

void CMOSSETrackerAppView::OnInputWebcam()
{
	if (cap.isOpened()) cap.release();
	cap.open(0);
	if (!cap.isOpened()) AfxMessageBox(_T("Failed to open webcam."));
	tracking = false;
	roiSelected = false;
	KillTimer(nTimer);
	SetTimer(nTimer, 33, NULL);
}

void CMOSSETrackerAppView::OnTrackingEnableSave()
{
	enableSaving = !enableSaving;
	CMenu* pMenu = AfxGetMainWnd()->GetMenu();
	if (pMenu)
		pMenu->CheckMenuItem(ID_TRACKING_ENABLESAVE, MF_BYCOMMAND | (enableSaving ? MF_CHECKED : MF_UNCHECKED));

	if (enableSaving) {
		// Ask for filename
		CFileDialog dlg(FALSE, _T("avi"), _T("tracked_output.avi"),
			OFN_OVERWRITEPROMPT, _T("AVI Files (.avi)|.avi||"));

		if (dlg.DoModal() == IDOK) {
			saveFilename = dlg.GetPathName();

			// ✅ Fix for conversion
			cv::String cvFilename = std::string(CT2A(saveFilename));

			videoWriter.open(cvFilename,
				cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
				30,
				currentFrame.size(),
				true);

			if (!videoWriter.isOpened()) {
				AfxMessageBox(_T("Failed to open video writer."));
				enableSaving = false;
				return;
			}
		}
		else {
			enableSaving = false; // Cancelled
			return;
		}
	}
	else {
		// Stop and release writer
		if (videoWriter.isOpened()) {
			videoWriter.release();
			AfxMessageBox(_T("Saving stopped."));
		}
	}
}

void CMOSSETrackerAppView::OnTrackingPauseResume()
{
	paused = !paused;

	CString status = paused ? _T("Tracking Paused") : _T("Tracking Resumed");
	AfxMessageBox(status);

	// Optional: Update button text (if you use a dynamic UI)
	CMenu* pMenu = AfxGetMainWnd()->GetMenu();
	if (pMenu) {
		CString label = paused ? _T("&Resume Tracking") : _T("&Pause Tracking");
		pMenu->ModifyMenu(ID_TRACKING_PAUSE_RESUME, MF_BYCOMMAND | MF_STRING, ID_TRACKING_PAUSE_RESUME, label);
	}
}

void CMOSSETrackerAppView::OnLButtonDown(UINT nFlags, CPoint point)
{
	if (currentFrame.empty()) return;
	selecting = true;
	selectionStart = point;
	selectionRect = CRect(point, point);
	CView::OnLButtonDown(nFlags, point);
}

void CMOSSETrackerAppView::OnLButtonUp(UINT nFlags, CPoint point)
{
	if (!selecting) return;
	selecting = false;
	selectionRect.BottomRight() = point;
	selectionRect.NormalizeRect();

	// Ensure minimum size
	if (selectionRect.Width() < 20 || selectionRect.Height() < 20) {
		AfxMessageBox(_T("Selection too small. Please select a larger area."));
		return;
	}

	// Clamp to frame bounds
	selectionRect.left = std::max(0L, std::min(selectionRect.left, (LONG)currentFrame.cols - 1));
	selectionRect.top = std::max(0L, std::min(selectionRect.top, (LONG)currentFrame.rows - 1));
	selectionRect.right = std::max(0L, std::min(selectionRect.right, (LONG)currentFrame.cols));
	selectionRect.bottom = std::max(0L, std::min(selectionRect.bottom, (LONG)currentFrame.rows));

	roi = Rect(selectionRect.left, selectionRect.top,
		selectionRect.Width(), selectionRect.Height());

	roiSelected = true;
	initMOSSE();
	tracking = true;

	// Debug output
	CString msg;
	msg.Format(_T("ROI selected: (%d,%d) %dx%d"), roi.x, roi.y, roi.width, roi.height);
	AfxMessageBox(msg);

	// Optional: Clear previous log data
	confidence_values.clear();

	Invalidate();
	CView::OnLButtonUp(nFlags, point);
}

void CMOSSETrackerAppView::OnMouseMove(UINT nFlags, CPoint point)
{
	if (selecting) {
		selectionRect.BottomRight() = point;
		Invalidate();
	}
	CView::OnMouseMove(nFlags, point);
}

void CMOSSETrackerAppView::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
{
	if (nChar == VK_SPACE) paused = !paused;
	else if (nChar == 'R') {
		tracking = false;
		roiSelected = false;

		if (!confidence_values.empty()) {
			// Ask user where to save log file
			CFileDialog saveDlg(FALSE, _T("csv"), _T("confidence_log.csv"),
				OFN_OVERWRITEPROMPT, _T("CSV Files (.csv)|.csv||"));

			if (saveDlg.DoModal() == IDOK) {
				CStdioFile logFile;
				if (logFile.Open(saveDlg.GetPathName(), CFile::modeCreate | CFile::modeWrite | CFile::typeText)) {
					logFile.WriteString(_T("Frame,Confidence,Timestamp (ms since start)\n"));
					for (int i = 0; i < confidence_values.size(); ++i) {
						CString line;
						line.Format(_T("%d,%.4f,%d\n"), i, confidence_values[i], timestamps_ms[i]);
						logFile.WriteString(line);
					}
					logFile.Close();
					AfxMessageBox(_T("Confidence log saved successfully."));
				}
				else {
					AfxMessageBox(_T("Failed to save confidence log."));
				}
			}
		}

		confidence_values.clear();
		timestamps_ms.clear();

		AfxMessageBox(_T("ROI reset. Please select a new region."));
	}
}

void CMOSSETrackerAppView::OnFilePrintPreview()
{
#ifndef SHARED_HANDLERS
	AFXPrintPreview(this);
#endif
}

BOOL CMOSSETrackerAppView::OnPreparePrinting(CPrintInfo* pInfo)
{
	return DoPreparePrinting(pInfo);
}

void CMOSSETrackerAppView::OnBeginPrinting(CDC* /pDC/, CPrintInfo* /pInfo/)
{
	// TODO: add initialization before printing
}

void CMOSSETrackerAppView::OnEndPrinting(CDC* /pDC/, CPrintInfo* /pInfo/)
{
	// TODO: cleanup after printing
}

void CMOSSETrackerAppView::OnRButtonUp(UINT /* nFlags */, CPoint point)
{
	ClientToScreen(&point);
	OnContextMenu(this, point);
}

void CMOSSETrackerAppView::OnContextMenu(CWnd* pWnd, CPoint point) {

}