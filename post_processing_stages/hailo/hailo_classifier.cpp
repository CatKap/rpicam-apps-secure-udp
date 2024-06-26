/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2024, Raspberry Pi Ltd
 *
 * hailo_classifier.cpp - Hailo inference for classifier network
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include <hailo/hailort.hpp>

#include "classification/classification.hpp"

#include "core/rpicam_app.hpp"

#include "hailo_postprocessing_stage.hpp"

using Size = libcamera::Size;
using PostProcFuncPtr = void (*)(HailoROIPtr);

#define NAME "hailo_classifier"
#define POSTPROC_LIB "libclassification.so"

static constexpr Size INPUT_TENSOR_SIZE { 224, 224 };

class HailoClassifier : public HailoPostProcessingStage
{
public:
	HailoClassifier(RPiCamApp *app);

	char const *Name() const override;

	void Read(boost::property_tree::ptree const &params) override;

	void Configure() override;

	bool Process(CompletedRequestPtr &completed_request) override;

private:
	std::vector<HailoClassificationPtr> runInference(uint8_t *frame);

	PostProcessingLib postproc_;

	// Config params
	float threshold_;
	bool do_softmax_;
};

HailoClassifier::HailoClassifier(RPiCamApp *app)
	: HailoPostProcessingStage(app), postproc_(PostProcLibDir(POSTPROC_LIB))
{
}

char const *HailoClassifier::Name() const
{
	return NAME;
}

void HailoClassifier::Read(boost::property_tree::ptree const &params)
{
	threshold_ = params.get<float>("threshold", 0.5f);
	do_softmax_ = params.get<bool>("do_softmax", true);

	HailoPostProcessingStage::Read(params);
}

void HailoClassifier::Configure()
{
	HailoPostProcessingStage::Configure();
}

bool HailoClassifier::Process(CompletedRequestPtr &completed_request)
{
	if (!HailoPostProcessingStage::Ready())
	{
		LOG_ERROR("HailoRT not ready!");
		return false;
	}

	std::shared_ptr<uint8_t> input;
	if (low_res_info_.pixel_format != libcamera::formats::BGR888)
	{
		StreamInfo rgb_info;
		rgb_info.width = INPUT_TENSOR_SIZE.width;
		rgb_info.height = INPUT_TENSOR_SIZE.height;
		rgb_info.stride = rgb_info.width * 3;

		BufferReadSync r(app_, completed_request->buffers[low_res_stream_]);
		libcamera::Span<uint8_t> buffer = r.Get()[0];
		input = allocator_.Allocate(rgb_info.stride * rgb_info.height);
		Yuv420ToRgb(input.get(), buffer.data(), low_res_info_, rgb_info);
	}

	std::vector<HailoClassificationPtr> results = runInference(input.get());
	if (results.size())
	{
		LOG(2, "Result: " << results[0]->get_label());
		completed_request->post_process_metadata.Set("annotate.text", results[0]->get_label());
	}

	return false;
}

std::vector<HailoClassificationPtr> HailoClassifier::runInference(uint8_t *frame)
{
	hailort::AsyncInferJob job;
	std::vector<OutTensor> output_tensors;
	hailo_status status;

	status = HailoPostProcessingStage::DispatchJob(frame, job, output_tensors);
	if (status != HAILO_SUCCESS)
		return {};

	// Wait for job completion.
	status = job.wait(1s);
	if (status != HAILO_SUCCESS)
	{
		LOG_ERROR("Failed to wait for inference to finish, status = " << status);
		return {};
	}

	// Postprocess tensor
	PostProcFuncPtr filter = reinterpret_cast<PostProcFuncPtr>(postproc_.GetSymbol("resnet_v1_50"));
	if (!filter)
		return {};

	HailoROIPtr roi = MakeROI(output_tensors);
	filter(roi);
	std::vector<HailoClassificationPtr> detections = hailo_common::get_hailo_classifications(roi);

	return detections;
}

static PostProcessingStage *Create(RPiCamApp *app)
{
	return new HailoClassifier(app);
}

static RegisterStage reg(NAME, &Create);
