﻿#region License
/*
Copyright © Joan Charmant 2015.
jcharmant@gmail.com 
 
This file is part of Kinovea.

Kinovea is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 
as published by the Free Software Foundation.

Kinovea is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Kinovea. If not, see http://www.gnu.org/licenses/.

*/
#endregion

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Linq;
using System.Windows.Forms;
using System.Xml;
using System.Xml.Serialization;

using Kinovea.ScreenManager.Languages;
using Kinovea.Services;

namespace Kinovea.ScreenManager
{
    [XmlType("TestGrid")]
    public class DrawingTestGrid : AbstractDrawing, IDecorable, IScalable
    {
        #region Properties
        public override string ToolDisplayName
        {
            get { return ScreenManagerLang.DrawingName_TestGrid; }
        }
        public override int ContentHash
        {
            get { return 0; }
        }
        public DrawingStyle DrawingStyle
        {
            get { return style; }
        }
        public override InfosFading InfosFading
        {
            get { return null; }
            set {  }
        }
        public override List<ToolStripItem> ContextMenu
        {
            get 
            {
                List<ToolStripItem> contextMenu = new List<ToolStripItem>();
                menuHide.Text = ScreenManagerLang.mnuCoordinateSystemHide;
                contextMenu.Add(menuHide);
                return contextMenu;
            }
        }
        public override DrawingCapabilities Caps
        {
            get { return DrawingCapabilities.ConfigureColorSize; }
        }
        public bool Visible { get; set; }
        #endregion

        #region Members
        private SizeF imageSize;
        
        private StyleHelper styleHelper = new StyleHelper();
        private DrawingStyle style;
        private Dictionary<string, GridLine> gridLines = new Dictionary<string, GridLine>();

        private ToolStripMenuItem menuHide = new ToolStripMenuItem();
        private static readonly log4net.ILog log = log4net.LogManager.GetLogger(System.Reflection.MethodBase.GetCurrentMethod().DeclaringType);
        #endregion

        #region Constructor
        public DrawingTestGrid(DrawingStyle preset = null)
        {
            CreateGridlines();

            // Decoration
            styleHelper.Color = Color.Red;
            styleHelper.HorizontalAxis = false;
            styleHelper.VerticalAxis = false;
            styleHelper.Frame = false;
            styleHelper.Thirds = false;
            if (preset != null)
            {
                style = preset.Clone();
                BindStyle();
            }
                
            menuHide.Click += menuHide_Click;
            menuHide.Image = Properties.Drawings.hide;
        }
        #endregion

        #region AbstractDrawing implementation
        public override void Draw(Graphics canvas, DistortionHelper distorter, IImageToViewportTransformer transformer, bool selected, long currentTimestamp)
        {
            if (!Visible || imageSize == SizeF.Empty)
                return;

            DrawGrid(canvas, transformer);
        }
        private void DrawGrid(Graphics canvas, IImageToViewportTransformer transformer)
        {
            //Pen pen = new Pen(Color.Red, 1);
            Pen p = styleHelper.GetPen(255);

            if (styleHelper.HorizontalAxis)
                DrawLine(canvas, transformer, p, gridLines["horizontal"]);
            
            if (styleHelper.VerticalAxis)    
                DrawLine(canvas, transformer, p, gridLines["vertical"]);
            
            if (styleHelper.Frame)
            {
                DrawLine(canvas, transformer, p, gridLines["frameLeft"]);
                DrawLine(canvas, transformer, p, gridLines["frameTop"]);
                DrawLine(canvas, transformer, p, gridLines["frameRight"]);
                DrawLine(canvas, transformer, p, gridLines["frameBottom"]);
            }
            
            if (styleHelper.Thirds)
            {
                DrawLine(canvas, transformer, p, gridLines["thirdsLeft"]);
                DrawLine(canvas, transformer, p, gridLines["thirdsTop"]);
                DrawLine(canvas, transformer, p, gridLines["thirdsRight"]);
                DrawLine(canvas, transformer, p, gridLines["thirdsBottom"]);
            }
            
            p.Dispose();
        }
        private void DrawLine(Graphics canvas, IImageToViewportTransformer transformer, Pen pen, GridLine gridLine)
        {
            canvas.DrawLine(pen, transformer.Transform(Map(gridLine.Start)), transformer.Transform(Map(gridLine.End)));
        }

        public override int HitTest(PointF point, long currentTimestamp, DistortionHelper distorter, IImageToViewportTransformer transformer, bool zooming)
        {
            bool hit = false;
            foreach (var pair in gridLines)
            {
                using (GraphicsPath path = new GraphicsPath())
                {
                    path.AddLine(Map(pair.Value.Start), Map(pair.Value.End));
                    hit = hit || HitTester.HitPath(point, path, 2, false, transformer);
                }
            }

            return hit ? 0 : -1;
        }
        public override void MoveDrawing(float dx, float dy, Keys modifierKeys, bool zooming)
        {
        }
        public override void MoveHandle(PointF point, int handleNumber, Keys modifiers)
        {
        }
        public override PointF GetCopyPoint()
        {
            return new PointF(imageSize.Width / 2, imageSize.Height / 2);
        }
        
        #endregion

        #region IScalable implementation
        public void Scale(Size imageSize)
        {
            this.imageSize = new SizeF(imageSize.Width, imageSize.Height);
        }
        #endregion

        #region Private methods
        private void BindStyle()
        {
            DrawingStyle.SanityCheck(style, ToolManager.GetStylePreset("TestGrid"));
            style.Bind(styleHelper, "Color", "color");
            style.Bind(styleHelper, "Toggles/HorizontalAxis", "horizontalAxis");
            style.Bind(styleHelper, "Toggles/VerticalAxis", "verticalAxis");
            style.Bind(styleHelper, "Toggles/Frame", "frame");
            style.Bind(styleHelper, "Toggles/Thirds", "thirds");
        }
        
        private void menuHide_Click(object sender, EventArgs e)
        {
            Visible = false;
            InvalidateFromMenu(sender);
        }

        private void CreateGridlines()
        {
            // Grid lines defined in normalized space [-1.0, +1.0].
            // +X left, +Y down.
            gridLines.Clear();
            
            // Main axes.
            gridLines.Add("horizontal", new GridLine(new PointF(-1, 0), new PointF(1, 0)));
            gridLines.Add("vertical", new GridLine(new PointF(0, -1), new PointF(0, 1)));
            
            // Safe framing.
            float margin = 0.8f;
            PointF a = new PointF(-margin, -margin);
            PointF b = new PointF(margin, -margin);
            PointF c = new PointF(margin, margin);
            PointF d = new PointF(-margin, margin);
            gridLines.Add("frameLeft", new GridLine(a, d));
            gridLines.Add("frameRight", new GridLine(b, c));
            gridLines.Add("frameTop", new GridLine(a, b));
            gridLines.Add("frameBottom", new GridLine(d, c));

            // Rule of thirds.
            gridLines.Add("thirdsLeft", new GridLine(new PointF(-1.0f / 3.0f, -1), new PointF(-1.0f / 3.0f, 1)));
            gridLines.Add("thirdsRight", new GridLine(new PointF(1.0f / 3.0f, -1), new PointF(1.0f / 3.0f, 1)));
            gridLines.Add("thirdsTop", new GridLine(new PointF(-1, -1.0f / 3.0f), new PointF(1, -1.0f / 3.0f)));
            gridLines.Add("thirdsBottom", new GridLine(new PointF(-1, 1.0f / 3.0f), new PointF(1, 1.0f / 3.0f)));
        }

        /// <summary>
        /// Go from normalized coordinates in [-1, +1] space to image coordinates.
        /// </summary>
        private PointF Map(PointF p)
        {
            return new PointF((p.X * 0.5f + 0.5f) * imageSize.Width, (p.Y * 0.5f + 0.5f) * imageSize.Height);
        }
        #endregion
    }
}
