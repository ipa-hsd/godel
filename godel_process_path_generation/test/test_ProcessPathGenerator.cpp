/*
 * test_ProcessPathGenerator.cpp
 *
 *  Created on: May 9, 2014
 *      Author: Dan Solomon
 */

#include <gtest/gtest.h>
#include "godel_process_path_generation/process_path_generator.h"

using godel_process_path::ProcessPathGenerator;
using godel_process_path::PolygonPt;

TEST(ProcessPathGeneratorTest, init)
{
  ProcessPathGenerator ppg;
  ppg.setTraverseHeight(.05);
  EXPECT_FALSE(ppg.createProcessPath());        // variables not properly set
  ppg.setMargin(-.01);
  ppg.setOverlap(.01);
  ppg.setToolRadius(.02);
  EXPECT_FALSE(ppg.createProcessPath());        // variables not properly set (margin)
  ppg.setMargin(0.);
  ppg.setOverlap(.04);
  ppg.setToolRadius(.02);
  EXPECT_FALSE(ppg.createProcessPath());        // variables not properly set (overlap)

  ppg.setMargin(.005);
  ppg.setOverlap(.01);
  ppg.setToolRadius(.025);
  EXPECT_FALSE(ppg.createProcessPath());        // Configure not done

  // Create single PolygonBoundary
//  godel_process_path::PolygonBoundaryCollection boundaries;
  godel_process_path::PolygonBoundary boundary;
  boundary.push_back(PolygonPt(0., 0.));
  boundary.push_back(PolygonPt(.1, 0.));
  boundary.push_back(PolygonPt(0., .1));
  ppg.verbose_ = true;
  EXPECT_TRUE(ppg.configure(godel_process_path::PolygonBoundaryCollection(1, boundary)));
  EXPECT_FALSE(ppg.createProcessPath());

  boundary.clear();
  boundary.push_back(PolygonPt(0., 0.));
  boundary.push_back(PolygonPt(.5, 0.));
  boundary.push_back(PolygonPt(0., .5));
  EXPECT_TRUE(ppg.configure(godel_process_path::PolygonBoundaryCollection(1, boundary)));
  EXPECT_TRUE(ppg.createProcessPath());
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
