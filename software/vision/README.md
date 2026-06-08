# Monopoly House Detection — Grove Vision AI V2

This folder contains the files for detecting **green houses** and **red hotels** on a Monopoly board using a custom-trained YOLOv8 object detection model.

## Files

| File | Description |
|------|-------------|
| `Monopoly_Detection_Swift_YOLO_192.ipynb` | Colab notebook for training the model and running inference to get detection results (bounding boxes, confidence scores, class labels) |
| `monopoly_full_int_vela.tflite` | Compiled model file for deployment on Grove Vision AI V2 (INT8 quantized, Vela-optimized) |

## What This Does

The model detects two classes on a Monopoly board:
- `green_house` — green house pieces
- `red_house` — red hotel pieces

Each detection returns:
```json
{
  "x": 120,
  "y": 85,
  "width": 30,
  "height": 30,
  "confidence": 0.93,
  "class": "green_house",
  "class_id": 0
}
```

## How to Use

### Run Inference (Notebook)
Open `Monopoly_Detection_Swift_YOLO_192.ipynb` in Google Colab to train the model or run inference on new images and get detection data.

### Deploy to Grove Vision AI V2
1. Open [SenseCraft Model Assistant](https://seeed-studio.github.io/SenseCraft-Web-Toolkit/#/setup/process) in Chrome
2. Connect your Grove Vision AI V2 via USB
3. Click **Upload Custom AI Model**
4. Upload `monopoly_full_int_vela.tflite`
5. Add labels in order: `green_house`, `red_house`
6. Click **Send Model**

## Model Performance

| Class | mAP50 | Precision | Recall |
|-------|-------|-----------|--------|
| green_house | 0.965 | 0.938 | 0.881 |
| red_house | 0.973 | 0.982 | 0.923 |
| **overall** | **0.969** | **0.960** | **0.902** |

## Hardware

- [Grove Vision AI V2](https://wiki.seeedstudio.com/grove_vision_ai_v2/)
- Compatible CSI camera (e.g. OV5647)

## Dataset

Labeled using [Roboflow](https://roboflow.com). Classes: `green_house` (989 annotations), `red_house` (309 annotations).
